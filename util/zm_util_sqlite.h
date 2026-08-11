#ifndef ZM_UTIL_SQLITE_H
#define ZM_UTIL_SQLITE_H

#include <sqlite3.h>

#include <mutex>
#include <string>

/**
 * @brief SQLite 轻量封装(连接 + 预处理语句 + bind 便捷)
 *
 * 仅做 RAII 与日志封装,不做行/列读取包装:业务代码直接经 ZmSqliteStmt::p
 * 调用 sqlite3_step / sqlite3_column_* / sqlite3_bind_*,与裸 SQLite 等价。
 *
 * 线程安全契约(重要):
 *   - 每连接一个 std::mutex,经 Mutex() 暴露给调用方;
 *   - 本类所有方法内部一律不加锁,由调用方在多语句序列外持锁
 *     (如 std::lock_guard lk(conn.Mutex())),与模块既有锁纪律一致
 *     (MSVC 对非递归互斥量重复加锁会抛异常,禁止类内加锁后调用方再加锁)。
 *
 * 连接默认打开参数:READWRITE | CREATE | FULLMUTEX(与业务现状一致;
 * 不做 journal/WAL/busy_timeout 设置,保持 SQLite 默认行为)。
 */
namespace zm {

/** @brief RAII 连接包装:打开/关闭/Exec + 错误日志(带 tag 前缀) */
class ZmSqliteConn
{
public:
    ZmSqliteConn();
    ~ZmSqliteConn();                       // 兜底 Close()
    ZmSqliteConn(const ZmSqliteConn&) = delete;
    ZmSqliteConn& operator=(const ZmSqliteConn&) = delete;

    /**
     * @brief 打开数据库(READWRITE|CREATE|FULLMUTEX)
     * @param logTag 日志前缀(如 "User"/"FileHub"),失败日志形如 [tag] 打开数据库失败
     * @return false 时已关残留句柄,可重试
     */
    bool Open(const std::string& path, const char* logTag);

    /** @brief 关闭连接(幂等) */
    void Close();

    bool IsOpen() const;

    /** @brief sqlite3_exec 执行;失败记 "[tag] exec failed: sql err=" 日志并返回 false */
    bool Exec(const char* sql);

    /**
     * @brief 事务辅助:BEGIN IMMEDIATE / COMMIT / ROLLBACK
     *        仅允许在调用方已持 Mutex() 的语句序列内配对使用(单连接串行,无并发代价);
     *        任一语句失败 → 调用方 Rollback 后返回错误,避免多语句中间态(崩溃自动回滚)
     */
    bool Begin();
    bool Commit();
    bool Rollback();

    /** @brief 替代裸调用 sqlite3_last_insert_rowid / sqlite3_changes / sqlite3_extended_errcode */
    int64_t LastInsertRowId() const;
    int     Changes() const;
    int     ExtendedErrorCode() const;

    /** @brief 底层句柄(供 ZmSqliteStmt 与需要裸调用的场景) */
    sqlite3* Raw() const;

    /** @brief 连接互斥锁:调用方自行持锁串行化语句序列 */
    std::mutex& Mutex();

private:
    sqlite3*    m_db = nullptr;
    std::mutex  m_mutex;
    std::string m_tag;
};

/** @brief RAII 预处理语句:prepare_v2(sql, -1, 无尾指针);失败时 p=nullptr,调用方判 p */
class ZmSqliteStmt
{
public:
    ZmSqliteStmt(ZmSqliteConn& conn, const char* sql);
    ~ZmSqliteStmt();                       // sqlite3_finalize
    ZmSqliteStmt(const ZmSqliteStmt&) = delete;
    ZmSqliteStmt& operator=(const ZmSqliteStmt&) = delete;

    sqlite3_stmt* p = nullptr;             // 直读:sqlite3_step / sqlite3_column_* / sqlite3_bind_*
};

/** @brief bind 便捷(SQLITE_TRANSIENT / sqlite3_bind_int64) */
void BindText(sqlite3_stmt* p, int idx, const std::string& v);
void BindInt(sqlite3_stmt* p, int idx, int64_t v);

} // namespace zm

#endif // ZM_UTIL_SQLITE_H

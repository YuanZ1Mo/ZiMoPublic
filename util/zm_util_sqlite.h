#ifndef ZM_UTIL_SQLITE_H
#define ZM_UTIL_SQLITE_H

// ============================================================================
// ZmSqliteDb:SQLite 连接与并发模型(公共库)
//  连接模型:写连接单条(可重入互斥保护)+ 读连接池(多读并发);WAL + busy_timeout。
//  线程模型:协程接口一律经工作池(事件循环离核)执行;*Sync 系列只在工作池线程
//            (WithTx 回调内)调用。
//  库表结构不在此层:建表/种子/清理规则由各自的 DbModule 子类承担。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct ZmSqliteConn;

class ZmSqliteDb
{
public:
    ZmSqliteDb();
    virtual ~ZmSqliteDb();

    /**
     * @brief 打开库并建立连接(写 1 + 读 readCount);父目录不存在时自动创建
     *
     * 只做准备动作,不建表;调用方(子类)在成功后自行设 PRAGMA、建表与种子。
     *
     * @param dbPath    库文件绝对路径
     * @param readCount 读连接数(默认 4)
     * @return true 打开成功;false 路径为空或连接创建失败
     */
    bool Init(const std::string& dbPath, int readCount = 4);

    /// @brief 关闭全部连接(可重复调用)
    void Close();

    /// @return 数据库是否可用(Init 失败/未调用为 false)
    bool IsReady() const { return m_ready.load(); }

    /// @return 库文件绝对路径(未 Init 时为空串)
    const std::string& DbPath() const { return m_dbPath; }

    // ── 协程数据访问(工作池离核;事件循环不阻塞) ──
    drogon::Task<bool> Exec(const std::string& sql,
                            const std::vector<std::string>& params = {});
    drogon::Task<ZMJSON> QueryRows(const std::string& sql,
                                   const std::vector<std::string>& params = {});
    drogon::Task<ZMJSON> QueryRow(const std::string& sql,
                                  const std::vector<std::string>& params = {});

    /**
     * @brief 事务封装:回调内写操作原子提交/回滚
     *
     * 回调在专用写线程(单写队列)执行,**回调内只能调用 *Sync 方法,不得再 co_await**;
     * 同一线程重入同一把递归锁,因此回调内调用其他模块的 *Sync 方法(如审计写入)
     * 会落在同一个事务里,不会另开连接。
     *
     * @param fn 事务体;返回 false 或抛异常 → 整体回滚
     * @return true 事务已提交;false 已回滚
     */
    drogon::Task<bool> WithTx(const std::function<bool(ZmSqliteDb&)>& fn);

    // ── 同步接口(仅 WithTx 回调 / 工作池上下文内调用) ──
    bool ExecSync(const std::string& sql, const std::vector<std::string>& params = {});
    ZMJSON QueryRowSync(const std::string& sql, const std::vector<std::string>& params = {});
    ZMJSON QueryRowsSync(const std::string& sql, const std::vector<std::string>& params = {});
    bool BeginTxSync();
    bool CommitTxSync();
    bool RollbackTxSync();

    /**
     * @brief 同步事务(已在工作池线程内时使用,省一次协程投递)
     *
     * 与 WithTx 同语义(回调内只能调 *Sync 且不得 co_await);语义同上,
     * **在事件循环线程上调用会阻塞事件循环**,只能在工作池线程内调用。
     *
     * @param fn 事务体;返回 false 或抛异常 → 整体回滚
     * @return true 已提交;false 已回滚
     */
    bool WithTxSync(const std::function<bool(ZmSqliteDb&)>& fn);

    /**
     * @brief 事务内读(走**写连接**,能看到本事务未提交的改动)
     *
     * 事务外的读走读连接池(QueryRow/QueryRows);事务内经读连接会读到事务开始前的
     * 快照(写事务未提交的数据读不到),因此事务内必须用本组方法。
     *
     * @param sql    SQL 文本
     * @param params 绑定参数
     * @return 单行 / 多行 JSON
     */
    ZMJSON QueryRowTxSync(const std::string& sql, const std::vector<std::string>& params = {});
    ZMJSON QueryRowsTxSync(const std::string& sql, const std::vector<std::string>& params = {});

    /// @return 当前 unix 秒
    static int64_t Now();

private:
    /// @brief 工作池线程内执行的同步执行体(调用方持锁)
    bool ExecSyncLocked(ZmSqliteConn* conn, const std::string& sql,
                        const std::vector<std::string>& params);
    ZMJSON QueryRowSyncLocked(ZmSqliteConn* conn, const std::string& sql,
                              const std::vector<std::string>& params);
    ZMJSON QueryRowsSyncLocked(ZmSqliteConn* conn, const std::string& sql,
                               const std::vector<std::string>& params);

    bool CreateConnections(int readCount);
    void CloseConnections();
    ZmSqliteConn* AcquireReadConn();
    void ReleaseReadConn(ZmSqliteConn* conn);

    // 连接(写 1 + 读池)
    std::unique_ptr<ZmSqliteConn> m_write;
    std::vector<std::unique_ptr<ZmSqliteConn>> m_reads;
    std::atomic<size_t> m_readCursor{0};
    std::atomic<bool> m_ready{false};
    std::string m_dbPath;
};

#endif /* ZM_UTIL_SQLITE_H */

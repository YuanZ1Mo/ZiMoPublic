#include "zm_util_sqlite.h"

#include "../spdlog/zm_logger.h"

namespace zm {

ZmSqliteConn::ZmSqliteConn() = default;

ZmSqliteConn::~ZmSqliteConn()
{
    Close();
}

bool ZmSqliteConn::Open(const std::string& path, const char* logTag)
{
    if (m_db)
        Close();
    m_tag = logTag ? logTag : "";

    if (sqlite3_open_v2(path.c_str(), &m_db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
    {
        DEFAULT_LOG_ERROR("[{}] 打开数据库失败: {} err={}", m_tag, path,
            m_db ? sqlite3_errmsg(m_db) : "?");
        if (m_db)
        {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
        return false;
    }
    return true;
}

void ZmSqliteConn::Close()
{
    if (m_db)
    {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool ZmSqliteConn::IsOpen() const
{
    return m_db != nullptr;
}

bool ZmSqliteConn::Exec(const char* sql)
{
    if (!m_db)
        return false;

    char* err = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        DEFAULT_LOG_ERROR("[{}] exec failed: {} err={}", m_tag, sql, err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

int64_t ZmSqliteConn::LastInsertRowId() const
{
    return m_db ? sqlite3_last_insert_rowid(m_db) : 0;
}

int ZmSqliteConn::Changes() const
{
    return m_db ? sqlite3_changes(m_db) : 0;
}

int ZmSqliteConn::ExtendedErrorCode() const
{
    return m_db ? sqlite3_extended_errcode(m_db) : 0;
}

sqlite3* ZmSqliteConn::Raw() const
{
    return m_db;
}

std::mutex& ZmSqliteConn::Mutex()
{
    return m_mutex;
}

ZmSqliteStmt::ZmSqliteStmt(ZmSqliteConn& conn, const char* sql)
{
    // 与模块原 Stmt 语义一致:prepare 失败不记日志,调用方判 p 处理
    if (sqlite3_prepare_v2(conn.Raw(), sql, -1, &p, nullptr) != SQLITE_OK)
        p = nullptr;
}

ZmSqliteStmt::~ZmSqliteStmt()
{
    if (p)
        sqlite3_finalize(p);
}

void BindText(sqlite3_stmt* p, int idx, const std::string& v)
{
    sqlite3_bind_text(p, idx, v.data(), (int)v.size(), SQLITE_TRANSIENT);
}

void BindInt(sqlite3_stmt* p, int idx, int64_t v)
{
    sqlite3_bind_int64(p, idx, v);
}

} // namespace zm

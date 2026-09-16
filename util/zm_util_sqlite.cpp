#include "zm_util_sqlite.h"

#include "zm_net_http_server.h"
#include "zm_util_logger.h"

#include <../drogon/include/sqlite3.h>

#include <ctime>
#include <filesystem>

// ============================================================================
// 内部连接结构
// ============================================================================
struct ZmSqliteConn
{
    sqlite3* db = nullptr;
    std::recursive_mutex mtx;   ///< 写连接可递归(WithTx 回调内再写)
};

namespace
{
/**
 * @brief 绑定参数(全部按文本绑定,SQLite 自动转换)
 *
 * @param stmt   已 prepare 的语句
 * @param params 参数列表(按 ?1..?N 顺序)
 */
void BindParams(sqlite3_stmt* stmt, const std::vector<std::string>& params)
{
    for (size_t i = 0; i < params.size(); ++i)
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1,
                          SQLITE_TRANSIENT);
}

/**
 * @brief 结果行 → JSON(列名→值;NULL→null;BLOB→hex 串)
 *
 * @param stmt 已 step 到行上的语句
 * @return 该行的 JSON 对象
 */
ZMJSON RowToJson(sqlite3_stmt* stmt)
{
    ZMJSON row = ZMJSON::object();
    int n = sqlite3_column_count(stmt);
    for (int i = 0; i < n; ++i)
    {
        const char* name = sqlite3_column_name(stmt, i);
        if (!name)
            continue;
        std::string key = name;
        switch (sqlite3_column_type(stmt, i))
        {
        case SQLITE_INTEGER:
            row[key] = sqlite3_column_int64(stmt, i);
            break;
        case SQLITE_FLOAT:
            row[key] = sqlite3_column_double(stmt, i);
            break;
        case SQLITE_TEXT:
            row[key] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            break;
        case SQLITE_BLOB:
        {
            const unsigned char* p = static_cast<const unsigned char*>(
                sqlite3_column_blob(stmt, i));
            int len = sqlite3_column_bytes(stmt, i);
            static const char* hex = "0123456789abcdef";
            std::string s;
            s.reserve(static_cast<size_t>(len) * 2);
            for (int j = 0; j < len; ++j)
            {
                s += hex[p[j] >> 4];
                s += hex[p[j] & 0x0F];
            }
            row[key] = s;
            break;
        }
        default:
            row[key] = nullptr;
            break;
        }
    }
    return row;
}

/**
 * @brief 执行写语句(不含事务控制)
 *
 * @param db     sqlite 句柄
 * @param sql    SQL 文本
 * @param params 绑定参数
 * @return true 执行完成(SQLITE_DONE/SQLITE_ROW)
 */
bool StepExec(sqlite3* db, const std::string& sql, const std::vector<std::string>& params)
{
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        DEFAULT_LOG_ERROR("DB prepare 失败 rc={} sql={} err={}", rc, sql,
                          sqlite3_errmsg(db));
        if (stmt)
            sqlite3_finalize(stmt);
        return false;
    }
    BindParams(stmt, params);
    rc = sqlite3_step(stmt);
    bool ok = (rc == SQLITE_DONE || rc == SQLITE_ROW);
    if (!ok)
        DEFAULT_LOG_ERROR("DB step 失败 rc={} sql={} err={}", rc, sql,
                          sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    return ok;
}
}  // namespace

// ============================================================================
// 构造 / 析构 / Init
// ============================================================================
ZmSqliteDb::ZmSqliteDb() = default;

ZmSqliteDb::~ZmSqliteDb()
{
    CloseConnections();
}

bool ZmSqliteDb::Init(const std::string& dbPath, int readCount)
{
    m_dbPath = dbPath;
    if (m_dbPath.empty())
    {
        DEFAULT_LOG_ERROR("ZmSqliteDb::Init: dbPath 为空");
        return false;
    }
    auto dir = std::filesystem::path(m_dbPath).parent_path();
    if (!dir.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            DEFAULT_LOG_ERROR("ZmSqliteDb::Init: 创建目录失败: {} ({})", dir.string(),
                              ec.message());
            return false;
        }
    }
    if (!CreateConnections(readCount < 1 ? 1 : readCount))
        return false;
    m_ready.store(true);
    return true;
}

void ZmSqliteDb::Close()
{
    CloseConnections();
    m_ready.store(false);
}

int64_t ZmSqliteDb::Now()
{
    return static_cast<int64_t>(std::time(nullptr));
}

// ============================================================================
// 连接管理
// ============================================================================
bool ZmSqliteDb::CreateConnections(int readCount)
{
    auto openOne = [this](const char* tag) -> ZmSqliteConn* {
        auto c = std::make_unique<ZmSqliteConn>();
        int rc = sqlite3_open_v2(m_dbPath.c_str(), &c->db,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (rc != SQLITE_OK)
        {
            DEFAULT_LOG_ERROR("ZmSqliteDb: sqlite3_open({}) 失败 rc={} err={}", tag,
                              rc, c->db ? sqlite3_errmsg(c->db) : "null");
            if (c->db)
                sqlite3_close(c->db);
            return nullptr;
        }
        sqlite3_busy_timeout(c->db, 5000);
        return c.release();
    };

    m_write.reset(openOne("write"));
    if (!m_write)
        return false;
    for (int i = 0; i < readCount; ++i)
    {
        ZmSqliteConn* c = openOne("read");
        if (!c)
            return false;
        m_reads.emplace_back(c);
    }
    return true;
}

void ZmSqliteDb::CloseConnections()
{
    if (m_write && m_write->db)
        sqlite3_close(m_write->db);
    m_write.reset();
    for (auto& c : m_reads)
    {
        if (c && c->db)
            sqlite3_close(c->db);
    }
    m_reads.clear();
}

ZmSqliteConn* ZmSqliteDb::AcquireReadConn()
{
    if (m_reads.empty())
        return nullptr;
    size_t idx = m_readCursor.fetch_add(1) % m_reads.size();
    return m_reads[idx].get();
}

void ZmSqliteDb::ReleaseReadConn(ZmSqliteConn* /*conn*/)
{
    // 读池连接常驻,无需归还动作
}

// ============================================================================
// 同步执行体(工作池线程 / WithTx 回调内调用)
// ============================================================================
bool ZmSqliteDb::ExecSyncLocked(ZmSqliteConn* conn, const std::string& sql,
                                const std::vector<std::string>& params)
{
    if (!conn || !conn->db)
        return false;
    return StepExec(conn->db, sql, params);
}

ZMJSON ZmSqliteDb::QueryRowSyncLocked(ZmSqliteConn* conn, const std::string& sql,
                                      const std::vector<std::string>& params)
{
    ZMJSON row = ZMJSON::object();
    if (!conn || !conn->db)
        return row;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        DEFAULT_LOG_ERROR("DB query prepare 失败 rc={} sql={} err={}", rc, sql,
                          sqlite3_errmsg(conn->db));
        if (stmt)
            sqlite3_finalize(stmt);
        return row;
    }
    BindParams(stmt, params);
    if (sqlite3_step(stmt) == SQLITE_ROW)
        row = RowToJson(stmt);
    sqlite3_finalize(stmt);
    return row;
}

ZMJSON ZmSqliteDb::QueryRowsSyncLocked(ZmSqliteConn* conn, const std::string& sql,
                                       const std::vector<std::string>& params)
{
    ZMJSON rows = ZMJSON::array();
    if (!conn || !conn->db)
        return rows;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        DEFAULT_LOG_ERROR("DB list prepare 失败 rc={} sql={} err={}", rc, sql,
                          sqlite3_errmsg(conn->db));
        if (stmt)
            sqlite3_finalize(stmt);
        return rows;
    }
    BindParams(stmt, params);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        rows.push_back(RowToJson(stmt));
    sqlite3_finalize(stmt);
    return rows;
}

bool ZmSqliteDb::ExecSync(const std::string& sql, const std::vector<std::string>& params)
{
    if (!m_write)
        return false;
    ZmSqliteConn* conn = m_write.get();
    std::lock_guard<std::recursive_mutex> lock(conn->mtx);
    return ExecSyncLocked(conn, sql, params);
}

ZMJSON ZmSqliteDb::QueryRowSync(const std::string& sql, const std::vector<std::string>& params)
{
    ZmSqliteConn* conn = AcquireReadConn();
    if (!conn)
        return ZMJSON::object();
    std::lock_guard<std::recursive_mutex> lock(conn->mtx);
    ZMJSON row = QueryRowSyncLocked(conn, sql, params);
    ReleaseReadConn(conn);
    return row;
}

ZMJSON ZmSqliteDb::QueryRowsSync(const std::string& sql, const std::vector<std::string>& params)
{
    ZmSqliteConn* conn = AcquireReadConn();
    if (!conn)
        return ZMJSON::array();
    std::lock_guard<std::recursive_mutex> lock(conn->mtx);
    ZMJSON rows = QueryRowsSyncLocked(conn, sql, params);
    ReleaseReadConn(conn);
    return rows;
}

bool ZmSqliteDb::BeginTxSync()
{
    if (!m_write)
        return false;
    ZmSqliteConn* conn = m_write.get();
    std::lock_guard<std::recursive_mutex> lock(conn->mtx);
    return ExecSyncLocked(conn, "BEGIN IMMEDIATE;", {});
}

bool ZmSqliteDb::CommitTxSync()
{
    if (!m_write)
        return false;
    ZmSqliteConn* conn = m_write.get();
    std::lock_guard<std::recursive_mutex> lock(conn->mtx);
    return ExecSyncLocked(conn, "COMMIT;", {});
}

bool ZmSqliteDb::RollbackTxSync()
{
    if (!m_write)
        return false;
    ZmSqliteConn* conn = m_write.get();
    std::lock_guard<std::recursive_mutex> lock(conn->mtx);
    return ExecSyncLocked(conn, "ROLLBACK;", {});
}

// ============================================================================
// 协程包装(工作池离核;事件循环不阻塞)
// ============================================================================
drogon::Task<bool> ZmSqliteDb::Exec(const std::string& sql,
                                    const std::vector<std::string>& params)
{
    co_return co_await ZmHttpServer::RunOnPool<bool>(
        [this, sql, params]() -> bool { return ExecSync(sql, params); });
}

drogon::Task<ZMJSON> ZmSqliteDb::QueryRows(const std::string& sql,
                                           const std::vector<std::string>& params)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, sql, params]() -> ZMJSON { return QueryRowsSync(sql, params); });
}

drogon::Task<ZMJSON> ZmSqliteDb::QueryRow(const std::string& sql,
                                          const std::vector<std::string>& params)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, sql, params]() -> ZMJSON { return QueryRowSync(sql, params); });
}

ZMJSON ZmSqliteDb::QueryRowTxSync(const std::string& sql,
                                  const std::vector<std::string>& params)
{
    if (!m_write)
        return ZMJSON::object();
    ZmSqliteConn* conn = m_write.get();
    std::lock_guard<std::recursive_mutex> lock(conn->mtx);
    return QueryRowSyncLocked(conn, sql, params);
}

ZMJSON ZmSqliteDb::QueryRowsTxSync(const std::string& sql,
                                   const std::vector<std::string>& params)
{
    if (!m_write)
        return ZMJSON::array();
    ZmSqliteConn* conn = m_write.get();
    std::lock_guard<std::recursive_mutex> lock(conn->mtx);
    return QueryRowsSyncLocked(conn, sql, params);
}

bool ZmSqliteDb::WithTxSync(const std::function<bool(ZmSqliteDb&)>& fn)
{
    if (!m_write)
        return false;
    ZmSqliteConn* conn = m_write.get();
    std::lock_guard<std::recursive_mutex> lock(conn->mtx);
    if (!ExecSyncLocked(conn, "BEGIN IMMEDIATE;", {}))
        return false;
    bool ok = false;
    try
    {
        ok = fn(*this);
    }
    catch (const std::exception& e)
    {
        DEFAULT_LOG_ERROR("ZmSqliteDb::WithTx 回调异常: {}", e.what());
        ok = false;
    }
    catch (...)
    {
        ok = false;
    }
    if (ok)
    {
        if (ExecSyncLocked(conn, "COMMIT;", {}))
            return true;
        ExecSyncLocked(conn, "ROLLBACK;", {});
        return false;
    }
    ExecSyncLocked(conn, "ROLLBACK;", {});
    return false;
}

drogon::Task<bool> ZmSqliteDb::WithTx(const std::function<bool(ZmSqliteDb&)>& fn)
{
    co_return co_await ZmHttpServer::RunOnPool<bool>([this, fn]() -> bool {
        return WithTxSync(fn);
    });
}

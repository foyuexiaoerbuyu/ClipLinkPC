// ---------------------------------------------------------------------------
// SQLite 统一访问层实现（见需求 §24-§26 / §59 / §72）
// ---------------------------------------------------------------------------

#include "database/DatabaseManager.h"

#include <windows.h>

#include <shlobj.h>

#include <cstdio>
#include <vector>

#include "util/Json.h"
#include "util/Logger.h"

namespace cliplink {
namespace database {

namespace {

constexpr const char* kTag = "DatabaseManager";

// §25 表结构 + §26 三条索引 + 本地状态表（lastServerSeq 持久化，§48）
const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS clipboard_history ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  event_id TEXT NOT NULL UNIQUE,"
    "  device_id TEXT NOT NULL,"
    "  content_hash TEXT NOT NULL,"
    "  content_type INTEGER NOT NULL,"
    "  content TEXT NOT NULL,"
    "  client_time INTEGER,"
    "  server_time INTEGER,"
    "  server_seq INTEGER,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_clipboard_created "
    "ON clipboard_history(created_at);"
    "CREATE INDEX IF NOT EXISTS idx_clipboard_hash "
    "ON clipboard_history(content_hash);"
    "CREATE INDEX IF NOT EXISTS idx_clipboard_server_seq "
    "ON clipboard_history(server_seq);"
    "CREATE TABLE IF NOT EXISTS app_state ("
    "  key TEXT PRIMARY KEY,"
    "  value INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ");";

// 本地时间戳 -> 文件名安全的备份后缀（UTC，仅用于损坏备份命名）
std::string corruptBackupName() {
    SYSTEMTIME st = {};
    GetSystemTime(&st);
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), ".corrupt-%04d%02d%02d-%02d%02d%02d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    return std::string(buf);
}

}  // namespace

// ---- Statement -------------------------------------------------------------

DatabaseManager::Statement::Statement(DatabaseManager& db, const char* sql)
    : db_(&db), lock_(db.lock()) {
    if (db_->handle() == nullptr) {
        return;  // 数据库不可用：语句保持 invalid，调用方按失败处理（§72）
    }
    const int rc =
        sqlite3_prepare_v2(db_->handle(), sql, -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        db_->lastError_ = sqlite3_errmsg(db_->handle());
        CLIPLOG_ERROR(kTag, std::string("prepare 失败: ") + db_->lastError_ +
                                ", sql=" + sql);
        stmt_ = nullptr;
    }
}

DatabaseManager::Statement::~Statement() {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
    }
}

int DatabaseManager::Statement::step() {
    if (stmt_ == nullptr) {
        return SQLITE_ERROR;
    }
    const int rc = sqlite3_step(stmt_);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        db_->lastError_ = sqlite3_errmsg(db_->handle());
        CLIPLOG_ERROR(kTag, std::string("step 失败: ") + db_->lastError_);
        sqlite3_reset(stmt_);
        return SQLITE_ERROR;
    }
    return rc;
}

bool DatabaseManager::Statement::bindInt64(int idx, int64_t value) {
    if (stmt_ == nullptr) return false;
    return sqlite3_bind_int64(stmt_, idx, value) == SQLITE_OK;
}

bool DatabaseManager::Statement::bindText(int idx, const std::string& value) {
    if (stmt_ == nullptr) return false;
    // SQLITE_TRANSIENT：sqlite 内部复制，绑定字符串可安全析构
    return sqlite3_bind_text(stmt_, idx, value.data(),
                             static_cast<int>(value.size()),
                             SQLITE_TRANSIENT) == SQLITE_OK;
}

bool DatabaseManager::Statement::bindNull(int idx) {
    if (stmt_ == nullptr) return false;
    return sqlite3_bind_null(stmt_, idx) == SQLITE_OK;
}

int64_t DatabaseManager::Statement::columnInt64(int idx) const {
    return stmt_ ? sqlite3_column_int64(stmt_, idx) : 0;
}

std::string DatabaseManager::Statement::columnText(int idx) const {
    if (stmt_ == nullptr) return {};
    const unsigned char* p = sqlite3_column_text(stmt_, idx);
    if (p == nullptr) return {};
    const int bytes = sqlite3_column_bytes(stmt_, idx);
    return std::string(reinterpret_cast<const char*>(p),
                       static_cast<size_t>(bytes));
}

bool DatabaseManager::Statement::columnNull(int idx) const {
    return stmt_ == nullptr ||
           sqlite3_column_type(stmt_, idx) == SQLITE_NULL;
}

int DatabaseManager::Statement::changes() const {
    return db_ && db_->handle() ? sqlite3_changes(db_->handle()) : 0;
}

// ---- DatabaseManager -------------------------------------------------------

DatabaseManager& DatabaseManager::instance() {
    static DatabaseManager inst;
    return inst;
}

std::string DatabaseManager::ensureDataDirLocked() {
    if (dirReady_) {
        return path_.empty() ? std::string() : path_.substr(
                   0, path_.find_last_of('/'));
    }
    wchar_t appdata[MAX_PATH] = {};
    const DWORD n = ExpandEnvironmentStringsW(L"%APPDATA%", appdata,
                                              MAX_PATH);
    if (n == 0 || n > MAX_PATH) {
        lastError_ = "ExpandEnvironmentStringsW(%APPDATA%) failed";
        return std::string();
    }
    std::wstring dir = std::wstring(appdata) + L"\\ClipLink";
    if (!CreateDirectoryW(dir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        lastError_ = "CreateDirectory failed, errno=" +
                     std::to_string(GetLastError());
        return std::string();
    }
    const std::string dirUtf8 = util::wideToUtf8(dir);
    dirReady_ = true;
    return dirUtf8;
}

bool DatabaseManager::openFileLocked() {
    const std::string dir = ensureDataDirLocked();
    if (dir.empty()) {
        CLIPLOG_ERROR(kTag, "数据目录不可用: " + lastError_);
        return false;
    }
    path_ = dir + "/cliplink.db";
    const std::wstring wpath = util::utf8ToWide(path_);

    sqlite3* raw = nullptr;
    const int rc = sqlite3_open16(reinterpret_cast<const void*>(wpath.c_str()),
                                   &raw);
    if (rc != SQLITE_OK) {
        lastError_ = raw ? sqlite3_errmsg(raw) : "sqlite3_open failed";
        if (raw) sqlite3_close(raw);
        CLIPLOG_ERROR(kTag, "打开数据库失败: " + lastError_);
        return false;
    }
    db_ = raw;
    sqlite3_busy_timeout(db_, 3000);
    return true;
}

bool DatabaseManager::probeLocked() {
    if (db_ == nullptr) return false;

    // 完整性检查：quick_check 返回 "ok" 才算通过
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA quick_check;", -1, &st, nullptr) !=
        SQLITE_OK) {
        return false;
    }
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* p = sqlite3_column_text(st, 0);
        ok = (p != nullptr &&
              std::string(reinterpret_cast<const char*>(p)) == "ok");
    }
    sqlite3_finalize(st);
    if (!ok) return false;

    // schema 探测：历史表必须可读
    const char* probeSql = "SELECT COUNT(*) FROM clipboard_history;";
    if (sqlite3_prepare_v2(db_, probeSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    ok = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return ok;
}

void DatabaseManager::quarantineLocked() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    if (path_.empty()) return;

    const std::wstring from = util::utf8ToWide(path_);
    const std::wstring to = from + util::utf8ToWide(corruptBackupName());
    if (MoveFileW(from.c_str(), to.c_str())) {
        CLIPLOG_WARN(kTag, "数据库文件损坏，已备份为: " +
                               util::wideToUtf8(to) + "，将重建新库");
    } else {
        // 备份失败（被占用等）：不删除原文件，直接降级内存，避免破坏数据
        CLIPLOG_ERROR(kTag, "数据库文件损坏且无法改名备份, errno=" +
                                std::to_string(GetLastError()));
    }
}

void DatabaseManager::degradeToMemoryLocked(const std::string& reason) {
    lastError_ = reason;
    sqlite3* raw = nullptr;
    if (sqlite3_open(":memory:", &raw) == SQLITE_OK && raw != nullptr) {
        db_ = raw;
        degraded_ = true;
        CLIPLOG_ERROR(kTag,
                      "数据库降级为内存模式（进程内可用，重启不保留）: " +
                          reason);
    } else {
        if (raw) sqlite3_close(raw);
        db_ = nullptr;
        available_ = false;
        CLIPLOG_ERROR(kTag, "内存数据库创建失败，数据库功能不可用: " + reason);
    }
}

bool DatabaseManager::initSchemaLocked() {
    std::string err;
    if (!exec(kSchemaSql, &err)) {
        lastError_ = err;
        return false;
    }
    // 兼容旧版表结构：缺少 sync_status 列时补齐（§85）
    bool hasSyncStatus = false;
    {
        Statement st(*this, "PRAGMA table_info(clipboard_history);");
        if (st.valid()) {
            while (st.step() == SQLITE_ROW) {
                if (st.columnText(1) == "sync_status") {
                    hasSyncStatus = true;
                    break;
                }
            }
        } else {
            return false;
        }
    }
    if (!hasSyncStatus &&
        !exec("ALTER TABLE clipboard_history "
              "ADD COLUMN sync_status INTEGER NOT NULL DEFAULT 0;",
              &err)) {
        lastError_ = err;
        return false;
    }
    return true;
}

bool DatabaseManager::open() {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    if (db_ != nullptr) {
        return available_;
    }

    if (openFileLocked()) {
        if (probeLocked()) {
            if (initSchemaLocked()) {
                available_ = true;
                degraded_ = false;
                CLIPLOG_INFO("DatabaseManager",
                             "数据库已打开: " + path_);
                return true;
            }
            // schema 初始化失败：按损坏处理
            quarantineLocked();
            if (openFileLocked() && initSchemaLocked()) {
                available_ = true;
                degraded_ = false;
                CLIPLOG_INFO(kTag, "数据库重建成功: " + path_);
                return true;
            }
        } else {
            // 已有文件打不开/损坏：备份后重建（§72 不崩溃）
            quarantineLocked();
            if (openFileLocked() && initSchemaLocked()) {
                available_ = true;
                degraded_ = false;
                CLIPLOG_INFO(kTag, "数据库重建成功: " + path_);
                return true;
            }
        }
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        degradeToMemoryLocked("文件库初始化失败: " + lastError_);
        return available_;
    }

    // 文件打开失败（目录不可写等）
    degradeToMemoryLocked("文件库打开失败: " + lastError_);
    return available_;
}

void DatabaseManager::close() {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    available_ = false;
    degraded_ = false;
}

bool DatabaseManager::available() const {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    return available_ && db_ != nullptr;
}

bool DatabaseManager::degraded() const {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    return degraded_;
}

const std::string& DatabaseManager::path() const {
    return path_;
}

bool DatabaseManager::exec(const std::string& sql, std::string* error) {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    if (db_ == nullptr) {
        if (error) *error = "database not available";
        return false;
    }
    char* errmsg = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        const std::string err = errmsg ? errmsg : "sqlite3_exec failed";
        if (errmsg) sqlite3_free(errmsg);
        lastError_ = err;
        if (error) *error = err;
        return false;
    }
    return true;
}

bool DatabaseManager::transaction(const std::function<bool()>& fn) {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    if (db_ == nullptr) return false;

    char* errmsg = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg) !=
        SQLITE_OK) {
        lastError_ = errmsg ? errmsg : "BEGIN failed";
        if (errmsg) sqlite3_free(errmsg);
        return false;
    }

    bool committed = false;
    try {
        committed = fn();
    } catch (const std::exception& ex) {
        lastError_ = std::string("transaction body exception: ") + ex.what();
        CLIPLOG_ERROR(kTag, lastError_);
        committed = false;
    } catch (...) {
        lastError_ = "transaction body unknown exception";
        CLIPLOG_ERROR(kTag, lastError_);
        committed = false;
    }

    sqlite3_exec(db_, committed ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr,
                 nullptr);
    return committed;
}

std::string DatabaseManager::lastError() const {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    return lastError_;
}

}  // namespace database
}  // namespace cliplink

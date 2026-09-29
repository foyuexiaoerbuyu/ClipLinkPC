#pragma once

// ---------------------------------------------------------------------------
// SQLite 统一访问层（见需求 §59 / §24）
// - 数据库文件：%APPDATA%\ClipLink\cliplink.db（不存在自动创建）
// - 建表 clipboard_history + 三条索引（created_at / content_hash / server_seq，
//   见 §25-§26），并提供本地状态表 app_state（lastServerSeq 持久化，§48）
// - 串行化：SQLITE_THREADSAFE=1 编译选项 + recursive_mutex 运行时串行化，
//   全部 SQLite 访问经本类封装，禁止项目各处直接执行 SQL（§59）
// - 统一事务入口 transaction()；Statement RAII 在整个生命周期内持锁
// - 降级策略（§72）：文件损坏时备份后重建，仍失败则降级 :memory:，
//   任何情况下不崩溃、不阻断程序启动
// ---------------------------------------------------------------------------

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "sqlite3.h"

namespace cliplink {
namespace database {

class DatabaseManager {
public:
    // ---- 预编译语句 RAII：构造即加锁 + prepare，析构即 finalize + 解锁 ----
    class Statement {
    public:
        Statement(DatabaseManager& db, const char* sql);
        ~Statement();

        Statement(const Statement&) = delete;
        Statement& operator=(const Statement&) = delete;

        bool valid() const { return stmt_ != nullptr; }

        // SQLITE_ROW / SQLITE_DONE / SQLITE_ERROR(-1)
        int step();

        bool bindInt64(int idx, int64_t value);
        bool bindText(int idx, const std::string& value);
        bool bindNull(int idx);

        int64_t    columnInt64(int idx) const;
        std::string columnText(int idx) const;
        bool       columnNull(int idx) const;

        // 最近一条受影响行数（INSERT/UPDATE/DELETE 后使用）
        int changes() const;

    private:
        DatabaseManager*  db_   = nullptr;
        sqlite3_stmt*     stmt_ = nullptr;
        std::unique_lock<std::recursive_mutex> lock_;
    };

    static DatabaseManager& instance();

    // 打开数据库并初始化 schema；失败按降级链处理，永不崩溃
    bool open();
    void close();

    bool available() const;  // 存在可用句柄（文件或内存降级）
    bool degraded() const;   // 当前处于 :memory: 降级模式

    // 数据库文件路径（UTF-8）
    const std::string& path() const;

    // 直接执行无结果 SQL（内部持锁）
    bool exec(const std::string& sql, std::string* error = nullptr);

    // 统一事务入口：fn 返回 true 提交，false 回滚；异常安全
    bool transaction(const std::function<bool()>& fn);

    // 供 Statement 构造/析构使用的锁接口
    std::unique_lock<std::recursive_mutex> lock() const { return std::unique_lock<std::recursive_mutex>(mutex_); }
    sqlite3* handle() const { return db_; }

    std::string lastError() const;

    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

private:
    DatabaseManager() = default;

    // 内部实现（调用方必须已持有 mutex_）
    bool openFileLocked();
    bool initSchemaLocked();
    // 探测已打开数据库是否可用（表可读、quick_check 通过）
    bool probeLocked();
    // 损坏处理：改名备份原文件，返回后重新 openFileLocked()
    void quarantineLocked();
    // 最终兜底：:memory: 降级
    void degradeToMemoryLocked(const std::string& reason);
    // %APPDATA%\ClipLink 目录（UTF-8），不存在则创建
    std::string ensureDataDirLocked();

    mutable std::recursive_mutex mutex_;
    sqlite3* db_        = nullptr;
    bool     available_ = false;
    bool     degraded_  = false;
    std::string path_;
    std::string lastError_;
    std::string dir_;
    bool dirReady_ = false;
};

}  // namespace database
}  // namespace cliplink

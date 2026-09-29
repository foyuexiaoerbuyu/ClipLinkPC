// ---------------------------------------------------------------------------
// 剪贴板历史仓库实现（见需求 §29 / §44 / §75 / §85-§87）
// ---------------------------------------------------------------------------

#include "database/ClipboardRepository.h"

#include "util/Logger.h"

namespace cliplink {
namespace database {

namespace {

constexpr const char* kTag = "ClipboardRepository";

constexpr int64_t kMillisPerDay = 24ll * 60ll * 60ll * 1000ll;

// 查询列（与 rowToEvent 的列索引一一对应）
const char* kSelectColumns =
    "event_id, device_id, content_hash, content_type, content, "
    "client_time, server_time, server_seq, created_at, sync_status "
    "FROM clipboard_history ";

}  // namespace

ClipboardRepository& ClipboardRepository::instance() {
    static ClipboardRepository inst;
    return inst;
}

ClipboardEvent ClipboardRepository::rowToEvent(
    DatabaseManager::Statement& st) {
    ClipboardEvent ev;
    ev.eventId     = st.columnText(0);
    ev.deviceId    = st.columnText(1);
    ev.contentHash = st.columnText(2);
    ev.contentType = static_cast<ContentType>(st.columnInt64(3));
    ev.content     = st.columnText(4);
    ev.clientTime  = st.columnInt64(5);
    ev.serverTime  = st.columnNull(6) ? 0 : st.columnInt64(6);
    ev.serverSeq   = st.columnNull(7) ? 0 : st.columnInt64(7);
    ev.createdAt   = st.columnInt64(8);
    ev.syncStatus  = static_cast<SyncStatus>(st.columnInt64(9));
    return ev;
}

InsertOutcome ClipboardRepository::insertEvent(const ClipboardEvent& event) {
    auto& db = DatabaseManager::instance();
    if (!db.available()) {
        CLIPLOG_ERROR(kTag, "insertEvent 跳过：数据库不可用");
        return InsertOutcome::Failed;
    }

    ClipboardEvent ev = event;
    if (ev.createdAt <= 0) {
        ev.createdAt = nowUnixMillis();
    }

    InsertOutcome outcome = InsertOutcome::Failed;
    const bool ok = db.transaction([&]() -> bool {
        // 1. eventId 幂等：已存在则不重复插入（§87 / §44）
        {
            DatabaseManager::Statement st(
                db, "SELECT 1 FROM clipboard_history WHERE event_id = ? "
                    "LIMIT 1;");
            if (!st.valid()) return false;
            st.bindText(1, ev.eventId);
            if (st.step() == SQLITE_ROW) {
                outcome = InsertOutcome::DuplicateEventId;
                return true;  // 无变更，提交即可
            }
        }

        // 2. 相邻最近一次 contentHash 相同则跳过（§29，非全表唯一）
        {
            DatabaseManager::Statement st(
                db, "SELECT content_hash FROM clipboard_history "
                    "ORDER BY id DESC LIMIT 1;");
            if (!st.valid()) return false;
            if (st.step() == SQLITE_ROW &&
                st.columnText(0) == ev.contentHash) {
                outcome = InsertOutcome::AdjacentDuplicate;
                return true;
            }
        }

        // 3. 插入；server_seq / server_time 未分配时存 NULL（§86）
        {
            DatabaseManager::Statement st(
                db,
                "INSERT INTO clipboard_history "
                "(event_id, device_id, content_hash, content_type, content, "
                " client_time, server_time, server_seq, created_at, "
                " sync_status) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
            if (!st.valid()) return false;
            st.bindText(1, ev.eventId);
            st.bindText(2, ev.deviceId);
            st.bindText(3, ev.contentHash);
            st.bindInt64(4, static_cast<int64_t>(ev.contentType));
            st.bindText(5, ev.content);
            st.bindInt64(6, ev.clientTime);
            if (ev.serverTime > 0) {
                st.bindInt64(7, ev.serverTime);
            } else {
                st.bindNull(7);
            }
            if (ev.serverSeq > 0) {
                st.bindInt64(8, ev.serverSeq);
            } else {
                st.bindNull(8);
            }
            st.bindInt64(9, ev.createdAt);
            st.bindInt64(10, static_cast<int64_t>(ev.syncStatus));
            if (st.step() != SQLITE_DONE) {
                outcome = InsertOutcome::Failed;
                return false;
            }
            outcome = InsertOutcome::Inserted;
            return true;
        }
    });

    if (!ok || outcome == InsertOutcome::Failed) {
        CLIPLOG_ERROR(kTag, "insertEvent 失败: " + db.lastError());
        return InsertOutcome::Failed;
    }
    if (outcome == InsertOutcome::Inserted) {
        notifyChanged();
    }
    return outcome;
}

std::vector<ClipboardEvent> ClipboardRepository::queryHistory(int offset,
                                                              int limit) {
    std::vector<ClipboardEvent> out;
    auto& db = DatabaseManager::instance();
    if (!db.available() || limit <= 0) return out;

    DatabaseManager::Statement st(
        db, (std::string("SELECT ") + kSelectColumns +
             "ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?;").c_str());
    if (!st.valid()) return out;
    st.bindInt64(1, limit);
    st.bindInt64(2, offset < 0 ? 0 : offset);
    while (st.step() == SQLITE_ROW) {
        out.push_back(rowToEvent(st));
    }
    return out;
}

bool ClipboardRepository::updateServerAck(const std::string& eventId,
                                          int64_t serverSeq,
                                          int64_t serverTime) {
    auto& db = DatabaseManager::instance();
    if (!db.available()) return false;

    DatabaseManager::Statement st(
        db,
        "UPDATE clipboard_history SET server_seq = ?, server_time = ?, "
        "sync_status = 1 WHERE event_id = ?;");
    if (!st.valid()) return false;
    st.bindInt64(1, serverSeq);
    st.bindInt64(2, serverTime);
    st.bindText(3, eventId);
    if (st.step() != SQLITE_DONE || st.changes() <= 0) {
        CLIPLOG_WARN(kTag, "updateServerAck 未命中 eventId=" + eventId);
        return false;
    }
    return true;
}

bool ClipboardRepository::markSyncFailed(const std::string& eventId) {
    auto& db = DatabaseManager::instance();
    if (!db.available()) return false;

    DatabaseManager::Statement st(
        db,
        "UPDATE clipboard_history SET sync_status = 2 "
        "WHERE event_id = ? AND server_seq IS NULL;");
    if (!st.valid()) return false;
    st.bindText(1, eventId);
    if (st.step() != SQLITE_DONE || st.changes() <= 0) {
        return false;
    }
    return true;
}

std::vector<ClipboardEvent> ClipboardRepository::queryPendingSync(int limit) {
    std::vector<ClipboardEvent> out;
    auto& db = DatabaseManager::instance();
    if (!db.available() || limit <= 0) return out;

    DatabaseManager::Statement st(
        db, (std::string("SELECT ") + kSelectColumns +
             "WHERE server_seq IS NULL AND sync_status IN (0, 2) "
             "ORDER BY id ASC LIMIT ?;").c_str());
    if (!st.valid()) return out;
    st.bindInt64(1, limit);
    while (st.step() == SQLITE_ROW) {
        out.push_back(rowToEvent(st));
    }
    return out;
}

int ClipboardRepository::cleanup(int maxHistoryCount, int maxHistoryDays) {
    auto& db = DatabaseManager::instance();
    if (!db.available()) return 0;

    int removed = 0;
    const bool ok = db.transaction([&]() -> bool {
        // 条件一：超过最长保存天数的过期记录（§28）
        if (maxHistoryDays > 0) {
            const int64_t deadline =
                nowUnixMillis() - static_cast<int64_t>(maxHistoryDays) *
                                      kMillisPerDay;
            DatabaseManager::Statement st(
                db, "DELETE FROM clipboard_history WHERE created_at < ?;");
            if (!st.valid()) return false;
            st.bindInt64(1, deadline);
            if (st.step() != SQLITE_DONE) return false;
            removed += st.changes();
        }

        // 条件二：超过最大条数时删除最旧记录（§28）
        if (maxHistoryCount > 0) {
            DatabaseManager::Statement st(
                db,
                "DELETE FROM clipboard_history WHERE id NOT IN "
                "(SELECT id FROM clipboard_history "
                " ORDER BY created_at DESC, id DESC LIMIT ?);");
            if (!st.valid()) return false;
            st.bindInt64(1, maxHistoryCount);
            if (st.step() != SQLITE_DONE) return false;
            removed += st.changes();
        }
        return true;
    });

    if (!ok) {
        CLIPLOG_ERROR(kTag, "cleanup 失败: " + db.lastError());
        return 0;
    }
    if (removed > 0) {
        CLIPLOG_INFO(kTag, "历史清理完成，删除 " + std::to_string(removed) +
                               " 条");
        notifyChanged();
    }
    return removed;
}

int64_t ClipboardRepository::count() {
    auto& db = DatabaseManager::instance();
    if (!db.available()) return 0;

    DatabaseManager::Statement st(
        db, "SELECT COUNT(*) FROM clipboard_history;");
    if (!st.valid()) return 0;
    return st.step() == SQLITE_ROW ? st.columnInt64(0) : 0;
}

void ClipboardRepository::setChangeCallback(ChangeCallback cb) {
    std::lock_guard<std::mutex> lock(changeMutex_);
    changeCallback_ = std::move(cb);
}

void ClipboardRepository::notifyChanged() {
    ChangeCallback cb;
    {
        std::lock_guard<std::mutex> lock(changeMutex_);
        cb = changeCallback_;
    }
    if (cb) cb();
}

}  // namespace database
}  // namespace cliplink

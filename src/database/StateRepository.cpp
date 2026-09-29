// ---------------------------------------------------------------------------
// 本地状态仓库实现（lastServerSeq 持久化，见需求 §48 / §50 / §110）
// ---------------------------------------------------------------------------

#include "database/StateRepository.h"

#include "common/DataTypes.h"
#include "database/DatabaseManager.h"
#include "util/Logger.h"

namespace cliplink {
namespace database {

namespace {
constexpr const char* kTag = "StateRepository";
}  // namespace

StateRepository& StateRepository::instance() {
    static StateRepository inst;
    return inst;
}

int64_t StateRepository::getLastServerSeq() {
    auto& db = DatabaseManager::instance();
    if (!db.available()) return 0;

    DatabaseManager::Statement st(
        db, "SELECT value FROM app_state WHERE key = ?;");
    if (!st.valid()) return 0;
    st.bindText(1, kKeyLastServerSeq);
    if (st.step() == SQLITE_ROW) {
        return st.columnInt64(0);
    }
    return 0;
}

bool StateRepository::setLastServerSeq(int64_t seq) {
    auto& db = DatabaseManager::instance();
    if (!db.available()) return false;
    if (seq < 0) return false;

    // 单调递增保护：不接受回退值，避免并发写入导致游标倒退
    if (seq < getLastServerSeq()) {
        return true;
    }

    DatabaseManager::Statement st(
        db,
        "INSERT INTO app_state (key, value, updated_at) VALUES (?, ?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, "
        "updated_at = excluded.updated_at;");
    if (!st.valid()) return false;
    st.bindText(1, kKeyLastServerSeq);
    st.bindInt64(2, seq);
    st.bindInt64(3, nowUnixMillis());
    if (st.step() != SQLITE_DONE) {
        CLIPLOG_ERROR(kTag, "setLastServerSeq 失败: " + db.lastError());
        return false;
    }
    return true;
}

}  // namespace database
}  // namespace cliplink

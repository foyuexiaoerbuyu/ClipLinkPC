#pragma once

// ---------------------------------------------------------------------------
// 剪贴板历史仓库（见需求 §25 / §28-§29 / §44 / §85-§86）
// - 幂等插入（event_id UNIQUE，重复 eventId 不产生新行，§87）
// - 相邻最近一次 contentHash 相同则跳过去重（§29，非全表唯一）
// - 按 created_at DESC 分页查询（§75）
// - 按 maxHistoryCount / maxHistoryDays 清理（§28，满足任意条件即清理）
// - serverSeq / sync_status 更新与待同步查询（§44 / §85 / §86）
// - 全部操作经 DatabaseManager 串行化，不可用时返回失败不崩溃（§72）
// ---------------------------------------------------------------------------

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "common/DataTypes.h"
#include "database/DatabaseManager.h"

namespace cliplink {
namespace database {

enum class InsertOutcome {
    Inserted,              // 新事件已入库
    DuplicateEventId,      // eventId 已存在（幂等命中，不重复插入）
    AdjacentDuplicate,     // 与最近一条 contentHash 相同，跳过（§29）
    Failed                 // 数据库不可用或执行失败
};

class ClipboardRepository {
public:
    static ClipboardRepository& instance();

    // 插入事件：eventId 幂等优先，其次相邻去重；createdAt 为空时取当前时间
    InsertOutcome insertEvent(const ClipboardEvent& event);

    // 按 created_at DESC 分页（新 -> 旧），offset/limit 由 UI 列表控制
    std::vector<ClipboardEvent> queryHistory(int offset, int limit);

    // 服务器 ACK：记录 serverSeq / serverTime，sync_status = SYNCED（§44）
    bool updateServerAck(const std::string& eventId, int64_t serverSeq,
                         int64_t serverTime);

    // 标记发送失败（sync_status = FAILED，保留 server_seq IS NULL 可重试，§85）
    bool markSyncFailed(const std::string& eventId);

    // 待同步事件：server_seq IS NULL 且状态 PENDING/FAILED，按 id 升序（§86）
    std::vector<ClipboardEvent> queryPendingSync(int limit);

    // 历史清理：超过 maxDays 的过期删除 + 超出 maxCount 的最旧删除（§28）
    // 参数 <= 0 表示该项条件不限制；返回删除行数
    int cleanup(int maxHistoryCount, int maxHistoryDays);

    // 记录总数（供 UI 与测试）
    int64_t count();

    // 变更通知（§63：Repository -> UI 刷新，禁止 UI 定时轮询 SELECT）
    // 在 insertEvent 产生 Inserted 与 cleanup 删除 >0 行后触发；
    // 回调可能在任意线程执行，实现方需自行线程安全（如 PostMessage）
    using ChangeCallback = std::function<void()>;
    void setChangeCallback(ChangeCallback cb);

    ClipboardRepository(const ClipboardRepository&) = delete;
    ClipboardRepository& operator=(const ClipboardRepository&) = delete;

private:
    ClipboardRepository() = default;

    static ClipboardEvent rowToEvent(DatabaseManager::Statement& st);
    void notifyChanged();

    std::mutex changeMutex_;
    ChangeCallback changeCallback_;
};

}  // namespace database
}  // namespace cliplink

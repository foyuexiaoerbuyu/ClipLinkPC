#pragma once

// ---------------------------------------------------------------------------
// 本地状态仓库（app_state 表）
// - lastServerSeq 持久化：已成功同步到的服务器全局序号（§48 / §110 客户端
//   重启后必须读回，不得丢失）
// - key / value 结构，后续可扩展其他本地状态
// - 数据库不可用时返回安全默认值，不崩溃（§72）
// ---------------------------------------------------------------------------

#include <cstdint>

namespace cliplink {
namespace database {

class StateRepository {
public:
    static StateRepository& instance();

    // 读取持久化的 lastServerSeq；不存在或不可用返回 0
    int64_t getLastServerSeq();

    // 持久化 lastServerSeq；仅允许单调递增（旧值更大时保留旧值）
    bool setLastServerSeq(int64_t seq);

    StateRepository(const StateRepository&) = delete;
    StateRepository& operator=(const StateRepository&) = delete;

private:
    StateRepository() = default;

    static constexpr const char* kKeyLastServerSeq = "lastServerSeq";
};

}  // namespace database
}  // namespace cliplink

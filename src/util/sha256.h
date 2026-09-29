#pragma once

// ---------------------------------------------------------------------------
// SHA-256（FIPS 180-4）
// 用途：contentHash 计算、防同步循环、历史去重（见需求 §14-§17）
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>

namespace cliplink {
namespace util {

class Sha256 {
public:
    Sha256();

    // 流式更新
    void update(const void* data, size_t len);

    // 结束计算并返回 64 位小写 hex；对象可复用（内部状态重置）
    std::string finalHex();

    // 一步计算：输入字符串 -> 64 位小写 hex
    static std::string hashHex(const std::string& input);
    static std::string hashHex(const void* data, size_t len);

private:
    void transform(const uint8_t block[64]);

    uint32_t state_[8];
    uint64_t totalLen_;
    uint8_t  buffer_[64];
    size_t   bufferLen_;
};

}  // namespace util
}  // namespace cliplink

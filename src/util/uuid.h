#pragma once

// ---------------------------------------------------------------------------
// UUID v4 生成（见需求 §18 eventId / §19 deviceId）
// ---------------------------------------------------------------------------

#include <string>

namespace cliplink {
namespace util {

// 生成小写标准格式 UUID v4，例如 "550e8400-e29b-41d4-a716-446655440000"
std::string uuidV4();

}  // namespace util
}  // namespace cliplink

#pragma once

// ---------------------------------------------------------------------------
// ClipLink 公共数据类型与常量
// 被 UI / Clipboard / Sync / Database 各模块共享，保持零业务依赖。
// 编码约定：所有网络与持久化文本统一 UTF-8；Windows Clipboard 为 UTF-16，
// 转换工具见 src/util/Json.h（wideToUtf8 / utf8ToWide）。
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstdint>
#include <string>

namespace cliplink {

// 当前 UTC Unix 毫秒时间戳（见需求 §80）
inline int64_t nowUnixMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 剪贴板事件来源（防循环同步核心枚举，见需求 §12）
enum class ClipboardSource {
    USER,    // 用户真实复制：保存历史 + 发送服务器
    REMOTE,  // 服务器同步来的：写入本地剪贴板，不回发
    HISTORY  // 点击本地历史：只写入剪贴板，不保存不发送
};

// 内容类型（MVP 仅 TEXT，保留扩展能力，见需求 §10）
enum class ContentType {
    TEXT  = 0,
    IMAGE = 1,
    FILE  = 2,
    HTML  = 3,
    RTF   = 4
};

// 离线事件同步状态（见需求 §85）
enum class SyncStatus {
    PENDING = 0,  // 尚未被服务器确认
    SYNCED  = 1,  // 已获得 serverSeq
    FAILED  = 2   // 发送失败，待重试
};

// 协议中 contentType 的字符串表示
inline const char* contentTypeToString(ContentType t) {
    switch (t) {
        case ContentType::IMAGE: return "IMAGE";
        case ContentType::FILE:  return "FILE";
        case ContentType::HTML:  return "HTML";
        case ContentType::RTF:   return "RTF";
        case ContentType::TEXT:
        default:                 return "TEXT";
    }
}

inline ContentType contentTypeFromString(const std::string& s) {
    if (s == "IMAGE") return ContentType::IMAGE;
    if (s == "FILE")  return ContentType::FILE;
    if (s == "HTML")  return ContentType::HTML;
    if (s == "RTF")   return ContentType::RTF;
    return ContentType::TEXT;
}

// 统一剪贴板事件（协议数据模型，见需求 §21）
struct ClipboardEvent {
    std::string eventId;      // UUID，全局唯一
    std::string deviceId;     // 本机 UUID，持久化
    std::string deviceType;   // "WINDOWS"（为 Android 预留）
    std::string deviceName;   // 设备显示名
    ContentType  contentType = ContentType::TEXT;
    std::string contentHash;  // SHA-256 hex（小写）
    std::string content;      // UTF-8 文本
    int64_t clientTime = 0;   // UTC 毫秒
    int64_t serverTime = 0;   // UTC 毫秒，服务器分配
    int64_t serverSeq  = 0;   // 服务器全局递增序号，0 = 尚未分配
    int64_t createdAt  = 0;   // 本地入库时间，UTC 毫秒
    ClipboardSource source = ClipboardSource::USER;
    SyncStatus syncStatus = SyncStatus::PENDING;
};

// 同步游标（离线增量同步，见需求 §48）
struct SyncCursor {
    int64_t lastServerSeq = 0;  // 已成功同步到的服务器序号
};

namespace constants {

// 剪贴板文本大小上限：1MB，超过不同步（见需求 §73）
constexpr size_t kMaxTextBytes = 1u * 1024u * 1024u;

// 历史记录默认上限（见需求 §27）
constexpr int kDefaultMaxHistoryCount = 1000;
constexpr int kDefaultMaxHistoryDays  = 30;

// WebSocket 默认端口（见需求 §34）
constexpr int kDefaultPort = 9000;

// 主窗口逻辑尺寸 200 x 500（见需求 §6）
constexpr int kMainWindowWidth  = 200;
constexpr int kMainWindowHeight = 500;

// 心跳间隔（Ping/Pong，秒，见需求 §40）
constexpr int kHeartbeatSeconds = 30;

// 日志文件滚动上限：5MB（见需求 §71）
constexpr uint64_t kMaxLogBytes = 5ull * 1024ull * 1024ull;

// 协议常量
constexpr const char* kDeviceTypeWindows = "WINDOWS";

}  // namespace constants
}  // namespace cliplink

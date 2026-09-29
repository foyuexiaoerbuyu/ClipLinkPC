#pragma once

// ---------------------------------------------------------------------------
// ClipLink 结构化同步协议（见需求 §21-§23 / §41-§46 / §50 / §80 / §100）
// - 统一信封：{"type": "...", "data": {...}}，禁止字符串裸匹配（§100）
// - 消息类型：register / clipboard / clipboard_ack / sync_request /
//   sync_response / error（§41-§44 / §50）
// - 编码统一 UTF-8；时间统一 UTC Unix 毫秒（§79-§80）
// - serverSeq 为全局同步顺序唯一依据，clientTime 仅作记录（§22-§23）
// - header-only：所有函数 inline，客户端与服务端共用同一套定义
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

#include "common/DataTypes.h"
#include "util/Json.h"

namespace cliplink {
namespace sync {

// ---- 消息类型（§41）-------------------------------------------------------
namespace msgtype {
inline constexpr const char* kRegister     = "register";
inline constexpr const char* kClipboard    = "clipboard";
inline constexpr const char* kClipboardAck = "clipboard_ack";
inline constexpr const char* kSyncRequest  = "sync_request";
inline constexpr const char* kSyncResponse = "sync_response";
inline constexpr const char* kError        = "error";
}  // namespace msgtype

enum class MessageType {
    Unknown = 0,
    Register,
    Clipboard,
    ClipboardAck,
    SyncRequest,
    SyncResponse,
    Error
};

inline MessageType messageTypeFromString(const std::string& s) {
    if (s == msgtype::kRegister)     return MessageType::Register;
    if (s == msgtype::kClipboard)    return MessageType::Clipboard;
    if (s == msgtype::kClipboardAck) return MessageType::ClipboardAck;
    if (s == msgtype::kSyncRequest)  return MessageType::SyncRequest;
    if (s == msgtype::kSyncResponse) return MessageType::SyncResponse;
    if (s == msgtype::kError)        return MessageType::Error;
    return MessageType::Unknown;
}

inline const char* messageTypeToString(MessageType t) {
    switch (t) {
        case MessageType::Register:     return msgtype::kRegister;
        case MessageType::Clipboard:    return msgtype::kClipboard;
        case MessageType::ClipboardAck: return msgtype::kClipboardAck;
        case MessageType::SyncRequest:  return msgtype::kSyncRequest;
        case MessageType::SyncResponse: return msgtype::kSyncResponse;
        case MessageType::Error:        return msgtype::kError;
        case MessageType::Unknown:      break;
    }
    return "unknown";
}

// ---- 解析结果 -------------------------------------------------------------
struct ParsedMessage {
    MessageType type = MessageType::Unknown;
    util::JsonValue data = util::JsonValue::makeObject();
};

// ---- 事件 <-> JSON（§21 数据模型）-----------------------------------------
// ClipboardEvent 序列化为协议 data；serverSeq / serverTime 仅在已分配时携带
// （0 = 未分配，不写入字段，见 §43 上行模型 / §46 下行模型）
inline util::JsonValue eventToJson(const ClipboardEvent& ev) {
    using util::JsonValue;
    JsonValue data = JsonValue::makeObject();
    data.set("eventId", ev.eventId);
    data.set("deviceId", ev.deviceId);
    data.set("deviceType", ev.deviceType);
    data.set("deviceName", ev.deviceName);
    data.set("contentType", std::string(contentTypeToString(ev.contentType)));
    data.set("contentHash", ev.contentHash);
    data.set("content", ev.content);
    data.set("clientTime", static_cast<int64_t>(ev.clientTime));
    if (ev.serverTime > 0) {
        data.set("serverTime", static_cast<int64_t>(ev.serverTime));
    }
    if (ev.serverSeq > 0) {
        data.set("serverSeq", static_cast<int64_t>(ev.serverSeq));
    }
    return data;
}

// 解析事件 data；校验失败返回 false 并给出 error 说明。
// 必填：eventId / deviceId / contentHash / content（§21 / §43 / §46）
inline bool eventFromJson(const util::JsonValue& data, ClipboardEvent& out,
                          std::string* error = nullptr) {
    using util::JsonValue;
    auto fail = [&](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (!data.isObject()) return fail("data 不是对象");

    out = ClipboardEvent();
    out.eventId     = data.get("eventId").asString();
    out.deviceId    = data.get("deviceId").asString();
    out.deviceType  = data.get("deviceType").asString();
    out.deviceName  = data.get("deviceName").asString();
    out.contentHash = data.get("contentHash").asString();
    out.clientTime  = data.get("clientTime").asInt64(0);
    out.serverTime  = data.get("serverTime").asInt64(0);
    out.serverSeq   = data.get("serverSeq").asInt64(0);
    out.contentType = contentTypeFromString(data.get("contentType").asString());
    out.syncStatus  = SyncStatus::SYNCED;  // 远端事件视为服务器已确认

    if (out.eventId.empty())    return fail("eventId 缺失");
    if (out.deviceId.empty())   return fail("deviceId 缺失");
    if (out.contentHash.empty()) return fail("contentHash 缺失");
    if (!data.has("content") || !data.get("content").isString()) {
        return fail("content 缺失");
    }
    out.content = data.get("content").asString();
    if (out.deviceType.empty()) out.deviceType = constants::kDeviceTypeWindows;
    if (out.deviceName.empty()) out.deviceName = out.deviceId;
    return true;
}

// ---- 信封构建（§41）-------------------------------------------------------
inline std::string wrapMessage(const std::string& type,
                               const util::JsonValue& data) {
    util::JsonValue env = util::JsonValue::makeObject();
    env.set("type", type);
    env.set("data", data);
    return env.dump(-1);  // 紧凑单行，UTF-8
}

// ---- 各消息构建 -----------------------------------------------------------

// register（§42）：连接建立后第一条消息，登记 deviceId -> 在线设备
inline std::string buildRegisterMessage(const std::string& deviceId,
                                        const std::string& deviceType,
                                        const std::string& deviceName) {
    util::JsonValue data = util::JsonValue::makeObject();
    data.set("deviceId", deviceId);
    data.set("deviceType", deviceType);
    data.set("deviceName", deviceName);
    return wrapMessage(msgtype::kRegister, data);
}

// clipboard 上行（§43）：USER 事件发送服务器
inline std::string buildClipboardMessage(const ClipboardEvent& ev) {
    return wrapMessage(msgtype::kClipboard, eventToJson(ev));
}

// clipboard 下行（§46）：服务器广播时必须携带 serverSeq（客户端校验单调）
inline std::string buildRemoteClipboardMessage(const ClipboardEvent& ev) {
    return wrapMessage(msgtype::kClipboard, eventToJson(ev));
}

// clipboard_ack（§44）：服务器 -> 来源客户端，携带 serverSeq / serverTime
inline std::string buildClipboardAckMessage(const std::string& eventId,
                                            int64_t serverSeq,
                                            int64_t serverTime) {
    util::JsonValue data = util::JsonValue::makeObject();
    data.set("eventId", eventId);
    data.set("serverSeq", static_cast<int64_t>(serverSeq));
    data.set("serverTime", static_cast<int64_t>(serverTime));
    return wrapMessage(msgtype::kClipboardAck, data);
}

// sync_request（§50）：增量补历史游标
inline std::string buildSyncRequestMessage(int64_t lastServerSeq) {
    util::JsonValue data = util::JsonValue::makeObject();
    data.set("lastServerSeq", static_cast<int64_t>(lastServerSeq));
    return wrapMessage(msgtype::kSyncRequest, data);
}

// sync_response（§50）：事件按 serverSeq ASC 排列；events 为空表示无缺口
inline std::string buildSyncResponseMessage(
    const std::vector<ClipboardEvent>& events) {
    util::JsonValue list = util::JsonValue::makeArray();
    for (const auto& ev : events) {
        list.push(eventToJson(ev));
    }
    util::JsonValue data = util::JsonValue::makeObject();
    data.set("events", list);
    return wrapMessage(msgtype::kSyncResponse, data);
}

// error（§72）：服务器对非法请求的结构化错误回复，绝不因错误请求断开退出
inline std::string buildErrorMessage(const std::string& code,
                                     const std::string& message) {
    util::JsonValue data = util::JsonValue::makeObject();
    data.set("code", code);
    data.set("message", message);
    return wrapMessage(msgtype::kError, data);
}

// ---- 信封解析（§100 结构化协议）------------------------------------------
inline bool parseMessage(const std::string& text, ParsedMessage& out,
                         std::string* error = nullptr) {
    util::JsonValue env;
    std::string jsonError;
    if (!util::JsonValue::parse(text, env, &jsonError)) {
        if (error != nullptr) *error = "JSON 解析失败: " + jsonError;
        return false;
    }
    if (!env.isObject()) {
        if (error != nullptr) *error = "消息不是 JSON 对象";
        return false;
    }
    const std::string type = env.get("type").asString();
    if (type.empty()) {
        if (error != nullptr) *error = "缺少 type 字段";
        return false;
    }
    out.type = messageTypeFromString(type);
    out.data = env.get("data");
    if (out.type == MessageType::Unknown) {
        if (error != nullptr) *error = "未知消息类型: " + type;
        return false;
    }
    if (!out.data.isObject()) {
        if (error != nullptr) *error = "缺少 data 对象";
        return false;
    }
    return true;
}

}  // namespace sync
}  // namespace cliplink

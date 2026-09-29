package com.cliplink.mobile.protocol;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.List;

/**
 * ClipLink 结构化同步协议（与 PC 端 src/sync/SyncProtocol.h 字段完全一致，
 * 见需求 §21-§23 / §41-§46 / §50 / §79-§80 / §100）。
 *
 * - 统一信封：{"type": "...", "data": {...}}，禁止字符串裸匹配（§100）
 * - 消息类型：register / clipboard / clipboard_ack / sync_request /
 *   sync_response / error（§41-§44 / §50）
 * - 编码统一 UTF-8（网络发送时 String -> UTF-8 字节）；
 *   时间统一 UTC Unix 毫秒（§79-§80）
 * - serverSeq 为全局同步顺序唯一依据，clientTime 仅作记录（§22-§23）
 */
public final class SyncProtocol {

    private SyncProtocol() {
    }

    // ---- 消息类型常量（§41）------------------------------------------------
    public static final String TYPE_REGISTER = "register";
    public static final String TYPE_CLIPBOARD = "clipboard";
    public static final String TYPE_CLIPBOARD_ACK = "clipboard_ack";
    public static final String TYPE_SYNC_REQUEST = "sync_request";
    public static final String TYPE_SYNC_RESPONSE = "sync_response";
    public static final String TYPE_ERROR = "error";

    /** Android 端设备类型标识（PC 端为 "WINDOWS"，见需求 §21） */
    public static final String DEVICE_TYPE_ANDROID = "ANDROID";

    /** 消息类型 */
    public enum MessageType {
        UNKNOWN,
        REGISTER,
        CLIPBOARD,
        CLIPBOARD_ACK,
        SYNC_REQUEST,
        SYNC_RESPONSE,
        ERROR;

        public static MessageType fromString(String s) {
            if (TYPE_REGISTER.equals(s)) return REGISTER;
            if (TYPE_CLIPBOARD.equals(s)) return CLIPBOARD;
            if (TYPE_CLIPBOARD_ACK.equals(s)) return CLIPBOARD_ACK;
            if (TYPE_SYNC_REQUEST.equals(s)) return SYNC_REQUEST;
            if (TYPE_SYNC_RESPONSE.equals(s)) return SYNC_RESPONSE;
            if (TYPE_ERROR.equals(s)) return ERROR;
            return UNKNOWN;
        }
    }

    /** 协议解析结果（type + data 对象） */
    public static final class ParsedMessage {
        public final MessageType type;
        public final JSONObject data;

        ParsedMessage(MessageType type, JSONObject data) {
            this.type = type;
            this.data = data;
        }
    }

    // ---- 事件 <-> JSON（§21 数据模型）--------------------------------------

    /**
     * ClipboardEvent 序列化为协议 data；serverSeq / serverTime 仅在已分配时携带
     * （0 = 未分配不写入字段，见 §43 上行模型 / §46 下行模型）。
     */
    public static JSONObject eventToJson(ClipboardEvent ev) throws JSONException {
        JSONObject data = new JSONObject();
        data.put("eventId", ev.getEventId());
        data.put("deviceId", ev.getDeviceId());
        data.put("deviceType", ev.getDeviceType());
        data.put("deviceName", ev.getDeviceName());
        data.put("contentType",
                ClipboardEvent.contentTypeToString(ev.getContentType()));
        data.put("contentHash", ev.getContentHash());
        data.put("content", ev.getContent());
        data.put("clientTime", ev.getClientTime());
        if (ev.getServerTime() > 0) {
            data.put("serverTime", ev.getServerTime());
        }
        if (ev.getServerSeq() > 0) {
            data.put("serverSeq", ev.getServerSeq());
        }
        return data;
    }

    /**
     * 解析事件 data；必填校验与 PC 端 eventFromJson 一致（§21 / §43 / §46）。
     *
     * @throws IllegalArgumentException 校验失败时抛出，message 为失败原因
     */
    public static ClipboardEvent eventFromJson(JSONObject data) {
        if (data == null) {
            throw new IllegalArgumentException("data 不是对象");
        }
        ClipboardEvent out = new ClipboardEvent();
        out.setEventId(data.optString("eventId", ""));
        out.setDeviceId(data.optString("deviceId", ""));
        out.setDeviceType(data.optString("deviceType", ""));
        out.setDeviceName(data.optString("deviceName", ""));
        out.setContentHash(data.optString("contentHash", ""));
        out.setClientTime(data.optLong("clientTime", 0L));
        out.setServerTime(data.optLong("serverTime", 0L));
        out.setServerSeq(data.optLong("serverSeq", 0L));
        out.setContentType(ClipboardEvent.contentTypeFromString(
                data.optString("contentType", "")));

        if (out.getEventId().isEmpty()) {
            throw new IllegalArgumentException("eventId 缺失");
        }
        if (out.getDeviceId().isEmpty()) {
            throw new IllegalArgumentException("deviceId 缺失");
        }
        if (out.getContentHash().isEmpty()) {
            throw new IllegalArgumentException("contentHash 缺失");
        }
        if (!data.has("content") || !(data.opt("content") instanceof String)) {
            throw new IllegalArgumentException("content 缺失");
        }
        out.setContent(data.optString("content", ""));
        if (out.getDeviceType().isEmpty()) {
            out.setDeviceType(DEVICE_TYPE_ANDROID);
        }
        if (out.getDeviceName().isEmpty()) {
            out.setDeviceName(out.getDeviceId());
        }
        // 远端事件视为服务器已确认（与 PC 端一致）
        out.setSyncStatus(ClipboardEvent.SYNC_STATUS_SYNCED);
        return out;
    }

    // ---- 信封构建（§41）----------------------------------------------------

    /**
     * 构建统一信封 JSON 字符串：{"type": "...", "data": {...}}（紧凑单行）。
     */
    public static String wrapMessage(String type, JSONObject data)
            throws JSONException {
        JSONObject env = new JSONObject();
        env.put("type", type);
        env.put("data", data);
        return env.toString();
    }

    // ---- 各消息构建 ---------------------------------------------------------

    /** register（§42）：连接建立后第一条消息，登记 deviceId -> 在线设备 */
    public static String buildRegisterMessage(String deviceId,
                                              String deviceType,
                                              String deviceName)
            throws JSONException {
        JSONObject data = new JSONObject();
        data.put("deviceId", deviceId);
        data.put("deviceType", deviceType);
        data.put("deviceName", deviceName);
        return wrapMessage(TYPE_REGISTER, data);
    }

    /** register 便捷重载：deviceType 固定为 "ANDROID" */
    public static String buildRegisterMessage(String deviceId, String deviceName)
            throws JSONException {
        return buildRegisterMessage(deviceId, DEVICE_TYPE_ANDROID, deviceName);
    }

    /** clipboard 上行（§43）：本机 USER 事件发送服务器 */
    public static String buildClipboardMessage(ClipboardEvent ev)
            throws JSONException {
        return wrapMessage(TYPE_CLIPBOARD, eventToJson(ev));
    }

    /** clipboard 下行（§46）：服务器广播时携带 serverSeq（客户端校验单调） */
    public static String buildRemoteClipboardMessage(ClipboardEvent ev)
            throws JSONException {
        return wrapMessage(TYPE_CLIPBOARD, eventToJson(ev));
    }

    /** clipboard_ack（§44）：携带 serverSeq / serverTime */
    public static String buildClipboardAckMessage(String eventId,
                                                  long serverSeq,
                                                  long serverTime)
            throws JSONException {
        JSONObject data = new JSONObject();
        data.put("eventId", eventId);
        data.put("serverSeq", serverSeq);
        data.put("serverTime", serverTime);
        return wrapMessage(TYPE_CLIPBOARD_ACK, data);
    }

    /** sync_request（§50）：增量补历史游标 */
    public static String buildSyncRequestMessage(long lastServerSeq)
            throws JSONException {
        JSONObject data = new JSONObject();
        data.put("lastServerSeq", lastServerSeq);
        return wrapMessage(TYPE_SYNC_REQUEST, data);
    }

    /** sync_response（§50）：事件按 serverSeq ASC 排列；空表示无缺口 */
    public static String buildSyncResponseMessage(List<ClipboardEvent> events)
            throws JSONException {
        JSONArray list = new JSONArray();
        if (events != null) {
            for (ClipboardEvent ev : events) {
                list.put(eventToJson(ev));
            }
        }
        JSONObject data = new JSONObject();
        data.put("events", list);
        return wrapMessage(TYPE_SYNC_RESPONSE, data);
    }

    /** error（§72）：结构化错误回复，绝不因错误请求断开退出 */
    public static String buildErrorMessage(String code, String message)
            throws JSONException {
        JSONObject data = new JSONObject();
        data.put("code", code);
        data.put("message", message);
        return wrapMessage(TYPE_ERROR, data);
    }

    // ---- 信封解析（§100 结构化协议）----------------------------------------

    /**
     * 解析信封：校验 JSON 对象、type 字段、已知类型与 data 对象。
     *
     * @throws IllegalArgumentException 解析或校验失败时抛出，message 为失败原因
     */
    public static ParsedMessage parseMessage(String text) {
        if (text == null || text.isEmpty()) {
            throw new IllegalArgumentException("消息为空");
        }
        JSONObject env;
        try {
            env = new JSONObject(text);
        } catch (JSONException e) {
            throw new IllegalArgumentException("JSON 解析失败: "
                    + e.getMessage());
        }
        String type = env.optString("type", "");
        if (type.isEmpty()) {
            throw new IllegalArgumentException("缺少 type 字段");
        }
        MessageType messageType = MessageType.fromString(type);
        if (messageType == MessageType.UNKNOWN) {
            throw new IllegalArgumentException("未知消息类型: " + type);
        }
        JSONObject data = env.optJSONObject("data");
        if (data == null) {
            throw new IllegalArgumentException("缺少 data 对象");
        }
        return new ParsedMessage(messageType, data);
    }
}

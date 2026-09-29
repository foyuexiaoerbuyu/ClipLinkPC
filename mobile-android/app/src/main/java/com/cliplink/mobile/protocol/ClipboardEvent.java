package com.cliplink.mobile.protocol;

/**
 * 统一剪贴板事件（协议数据模型，与 PC 端 src/common/DataTypes.h 的
 * ClipboardEvent 字段一一对应，见需求 §21）。
 *
 * 内容类型与同步状态在本地数据库中以 INTEGER 持久化（见需求 §25 / §85），
 * 在协议中 contentType 以字符串传输（见 PC 端 SyncProtocol.h eventToJson）。
 */
public final class ClipboardEvent {

    // ---- 内容类型（见需求 §10，MVP 仅 TEXT，保留扩展能力）----
    public static final int CONTENT_TYPE_TEXT = 0;
    public static final int CONTENT_TYPE_IMAGE = 1;
    public static final int CONTENT_TYPE_FILE = 2;
    public static final int CONTENT_TYPE_HTML = 3;
    public static final int CONTENT_TYPE_RTF = 4;

    // ---- 离线事件同步状态（见需求 §85）----
    public static final int SYNC_STATUS_PENDING = 0; // 尚未被服务器确认
    public static final int SYNC_STATUS_SYNCED = 1;  // 已获得 serverSeq
    public static final int SYNC_STATUS_FAILED = 2;  // 发送失败，待重试

    /** 协议中 contentType 的字符串表示（与 PC 端 contentTypeToString 一致） */
    public static String contentTypeToString(int contentType) {
        switch (contentType) {
            case CONTENT_TYPE_IMAGE:
                return "IMAGE";
            case CONTENT_TYPE_FILE:
                return "FILE";
            case CONTENT_TYPE_HTML:
                return "HTML";
            case CONTENT_TYPE_RTF:
                return "RTF";
            case CONTENT_TYPE_TEXT:
            default:
                return "TEXT";
        }
    }

    /** 协议字符串 -> 本地 INTEGER（与 PC 端 contentTypeFromString 一致） */
    public static int contentTypeFromString(String s) {
        if ("IMAGE".equals(s)) return CONTENT_TYPE_IMAGE;
        if ("FILE".equals(s)) return CONTENT_TYPE_FILE;
        if ("HTML".equals(s)) return CONTENT_TYPE_HTML;
        if ("RTF".equals(s)) return CONTENT_TYPE_RTF;
        return CONTENT_TYPE_TEXT;
    }

    private String eventId = "";
    private String deviceId = "";
    private String deviceType = "";
    private String deviceName = "";
    private int contentType = CONTENT_TYPE_TEXT;
    private String contentHash = "";
    private String content = "";
    private long clientTime = 0L;   // UTC 毫秒
    private long serverTime = 0L;   // UTC 毫秒，服务器分配，0 = 未分配
    private long serverSeq = 0L;    // 服务器全局递增序号，0 = 未分配
    private long createdAt = 0L;    // 本地入库时间，UTC 毫秒
    private int syncStatus = SYNC_STATUS_PENDING;

    public ClipboardEvent() {
    }

    public String getEventId() {
        return eventId;
    }

    public void setEventId(String eventId) {
        this.eventId = eventId == null ? "" : eventId;
    }

    public String getDeviceId() {
        return deviceId;
    }

    public void setDeviceId(String deviceId) {
        this.deviceId = deviceId == null ? "" : deviceId;
    }

    public String getDeviceType() {
        return deviceType;
    }

    public void setDeviceType(String deviceType) {
        this.deviceType = deviceType == null ? "" : deviceType;
    }

    public String getDeviceName() {
        return deviceName;
    }

    public void setDeviceName(String deviceName) {
        this.deviceName = deviceName == null ? "" : deviceName;
    }

    public int getContentType() {
        return contentType;
    }

    public void setContentType(int contentType) {
        this.contentType = contentType;
    }

    public String getContentHash() {
        return contentHash;
    }

    public void setContentHash(String contentHash) {
        this.contentHash = contentHash == null ? "" : contentHash;
    }

    public String getContent() {
        return content;
    }

    public void setContent(String content) {
        this.content = content == null ? "" : content;
    }

    public long getClientTime() {
        return clientTime;
    }

    public void setClientTime(long clientTime) {
        this.clientTime = clientTime;
    }

    public long getServerTime() {
        return serverTime;
    }

    public void setServerTime(long serverTime) {
        this.serverTime = serverTime;
    }

    public long getServerSeq() {
        return serverSeq;
    }

    public void setServerSeq(long serverSeq) {
        this.serverSeq = serverSeq;
    }

    public long getCreatedAt() {
        return createdAt;
    }

    public void setCreatedAt(long createdAt) {
        this.createdAt = createdAt;
    }

    public int getSyncStatus() {
        return syncStatus;
    }

    public void setSyncStatus(int syncStatus) {
        this.syncStatus = syncStatus;
    }

    @Override
    public String toString() {
        return "ClipboardEvent{eventId='" + eventId + "', deviceType='"
                + deviceType + "', contentType=" + contentType
                + ", contentHash='" + contentHash + "', serverSeq="
                + serverSeq + ", syncStatus=" + syncStatus + '}';
    }
}

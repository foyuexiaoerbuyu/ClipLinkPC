package com.cliplink.mobile.data;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;

import com.cliplink.mobile.protocol.UuidUtil;

/**
 * 配置仓库（SharedPreferences 持久化，见需求 §27 / §37 / §90）。
 *
 * - deviceId：首次启动生成 UUID 并持久化，重启不变（§19 / §102）
 * - deviceName：默认取 Build.MODEL（对应 PC 端默认 Windows 计算机名，§20）
 * - serverAddress / port：禁止写死，统一从配置读取（§101）
 * - autoSync / maxHistoryCount / maxHistoryDays（§27-§28）
 */
public class ConfigRepository {

    private static final String PREFS_NAME = "cliplink_config";

    private static final String KEY_DEVICE_ID = "deviceId";
    private static final String KEY_DEVICE_NAME = "deviceName";
    private static final String KEY_SERVER_ADDRESS = "serverAddress";
    private static final String KEY_PORT = "port";
    private static final String KEY_AUTO_SYNC = "autoSync";
    private static final String KEY_MAX_HISTORY_COUNT = "maxHistoryCount";
    private static final String KEY_MAX_HISTORY_DAYS = "maxHistoryDays";

    /** 默认配置（与 PC 端 constants 对齐，见需求 §27 / §34） */
    public static final String DEFAULT_SERVER_ADDRESS = "127.0.0.1";
    public static final int DEFAULT_PORT = 9000;
    public static final boolean DEFAULT_AUTO_SYNC = true;
    public static final int DEFAULT_MAX_HISTORY_COUNT = 1000;
    public static final int DEFAULT_MAX_HISTORY_DAYS = 30;

    private final SharedPreferences prefs;

    public ConfigRepository(Context context) {
        this.prefs = context.getApplicationContext()
                .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }

    /**
     * 设备唯一标识：首次调用时生成 UUID v4 并持久化，之后恒定不变（§19）。
     */
    public synchronized String getDeviceId() {
        String deviceId = prefs.getString(KEY_DEVICE_ID, "");
        if (deviceId == null || deviceId.isEmpty()) {
            deviceId = UuidUtil.uuidV4();
            prefs.edit().putString(KEY_DEVICE_ID, deviceId).apply();
        }
        return deviceId;
    }

    /** 设备显示名，默认 Build.MODEL（§20） */
    public String getDeviceName() {
        String name = prefs.getString(KEY_DEVICE_NAME, "");
        if (name == null || name.isEmpty()) {
            return Build.MODEL == null ? "Android" : Build.MODEL;
        }
        return name;
    }

    public void setDeviceName(String deviceName) {
        prefs.edit().putString(KEY_DEVICE_NAME,
                deviceName == null ? "" : deviceName).apply();
    }

    /** 服务器地址（主机或 ws:// 完整地址，见需求 §37 / §101） */
    public String getServerAddress() {
        String address = prefs.getString(KEY_SERVER_ADDRESS, "");
        if (address == null || address.isEmpty()) {
            return DEFAULT_SERVER_ADDRESS;
        }
        return address;
    }

    public void setServerAddress(String serverAddress) {
        prefs.edit().putString(KEY_SERVER_ADDRESS,
                serverAddress == null ? "" : serverAddress).apply();
    }

    /** 服务器端口（见需求 §34 / §101） */
    public int getPort() {
        return prefs.getInt(KEY_PORT, DEFAULT_PORT);
    }

    public void setPort(int port) {
        prefs.edit().putInt(KEY_PORT, port).apply();
    }

    /**
     * 组装 WebSocket 连接地址：配置值已含协议头时原样使用，
     * 否则拼接 ws://host:port（支持 ws:// 与 wss://，见需求 §37）。
     */
    public String getServerUrl() {
        String address = getServerAddress();
        if (address.startsWith("ws://") || address.startsWith("wss://")) {
            return address;
        }
        return "ws://" + address + ":" + getPort();
    }

    /** 自动同步开关（见需求 §36） */
    public boolean getAutoSync() {
        return prefs.getBoolean(KEY_AUTO_SYNC, DEFAULT_AUTO_SYNC);
    }

    public void setAutoSync(boolean autoSync) {
        prefs.edit().putBoolean(KEY_AUTO_SYNC, autoSync).apply();
    }

    /** 历史最大条数（见需求 §27-§28） */
    public int getMaxHistoryCount() {
        return prefs.getInt(KEY_MAX_HISTORY_COUNT, DEFAULT_MAX_HISTORY_COUNT);
    }

    public void setMaxHistoryCount(int maxHistoryCount) {
        prefs.edit().putInt(KEY_MAX_HISTORY_COUNT, maxHistoryCount).apply();
    }

    /** 历史最长保存天数（见需求 §27-§28） */
    public int getMaxHistoryDays() {
        return prefs.getInt(KEY_MAX_HISTORY_DAYS, DEFAULT_MAX_HISTORY_DAYS);
    }

    public void setMaxHistoryDays(int maxHistoryDays) {
        prefs.edit().putInt(KEY_MAX_HISTORY_DAYS, maxHistoryDays).apply();
    }
}

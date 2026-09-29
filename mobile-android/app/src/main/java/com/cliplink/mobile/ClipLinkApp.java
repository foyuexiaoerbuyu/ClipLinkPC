package com.cliplink.mobile;

import android.app.Application;
import android.util.Log;

import com.cliplink.mobile.data.ClipboardRepository;
import com.cliplink.mobile.data.ConfigRepository;
import com.cliplink.mobile.data.DatabaseHelper;

/**
 * ClipLink 应用入口：初始化配置与数据库并输出启动日志（见需求 §59 / §71）。
 * 后续轮次的同步服务 / UI 通过 getInstance() 获取统一的数据层实例。
 */
public class ClipLinkApp extends Application {

    private static final String TAG = "ClipLink";

    private static ClipLinkApp instance;

    private ConfigRepository config;
    private ClipboardRepository clipboardRepository;

    public static ClipLinkApp getInstance() {
        return instance;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;

        config = new ConfigRepository(this);
        clipboardRepository = new ClipboardRepository(this);

        // 启动日志：确认 deviceId 已持久化、数据库可用、服务器地址就绪
        Log.i(TAG, "ClipLink 启动, deviceId=" + config.getDeviceId()
                + ", deviceName=" + config.getDeviceName()
                + ", server=" + config.getServerUrl()
                + ", autoSync=" + config.getAutoSync());

        // 数据库连通性自检：仅为启动诊断，任何数据库异常不得让
        // Application 创建失败（§72 程序不能因数据库问题退出）。
        // 异常完整记录到日志，不静默吞掉，便于排查真实问题。
        try {
            Log.i(TAG, "数据库 " + DatabaseHelper.DB_NAME
                    + " 历史记录 " + clipboardRepository.count() + " 条"
                    + ", lastServerSeq=" + clipboardRepository.getLastServerSeq());
        } catch (RuntimeException e) {
            Log.e(TAG, "数据库启动自检失败（应用继续启动，后续操作会重试）: "
                    + e.getMessage(), e);
        }
    }

    public ConfigRepository getConfig() {
        return config;
    }

    public ClipboardRepository getClipboardRepository() {
        return clipboardRepository;
    }
}

package com.cliplink.mobile.service;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

import com.cliplink.mobile.sync.SyncManager;

/**
 * 剪贴板同步前台服务（见需求 §91-§92）。
 *
 * Android 不允许永久后台监听剪贴板，通信与同步需以前台服务承载。
 * 服务真实接线：启动 SyncManager（创建 WebSocket 连接 + 剪贴板监听启停），
 * 连接/暂停/历史状态经 SyncManager 静态监听器通知 UI（§63，不用 EventBus）。
 */
public class SyncForegroundService extends Service {

    private static final String TAG = "ClipLink";
    private static final String CHANNEL_ID = "cliplink_sync";
    private static final int NOTIFICATION_ID = 1001;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
        startForeground(NOTIFICATION_ID, buildNotification());
        // 真实接线：创建 SyncManager 并连接、启动剪贴板监听（§81-§84）
        SyncManager.getInstance().start(this);
    }

    @Override
    public void onDestroy() {
        // 停止 WebSocket 与剪贴板监听（对应 §11 RemoveClipboardFormatListener）
        SyncManager.getInstance().stop();
        super.onDestroy();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // 服务被系统回收后重建（START_STICKY）时，主动催一次连接自愈（§72 / §91）
        SyncManager.getInstance().ensureConnected();
        // 剪贴板同步需长期存活：被系统回收后自动重建（见需求 §72）
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID,
                    "剪贴板同步",
                    NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("ClipLink 同步服务常驻通知");
            NotificationManager manager =
                    getSystemService(NotificationManager.class);
            if (manager != null) {
                manager.createNotificationChannel(channel);
            }
        }
    }

    private Notification buildNotification() {
        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            builder = new Notification.Builder(this, CHANNEL_ID);
        } else {
            builder = new Notification.Builder(this);
        }
        return builder
                .setContentTitle("ClipLink")
                .setContentText("剪贴板同步服务运行中")
                .setSmallIcon(android.R.drawable.ic_menu_manage)
                .setOngoing(true)
                .build();
    }
}

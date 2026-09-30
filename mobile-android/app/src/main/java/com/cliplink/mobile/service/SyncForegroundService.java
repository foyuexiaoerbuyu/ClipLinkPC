package com.cliplink.mobile.service;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import androidx.core.content.ContextCompat;

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

    /** 保活自拉起使用的 action（第二批 §11，无副作用，仅用于重入 onStartCommand） */
    public static final String ACTION_KEEP_ALIVE =
            "com.cliplink.mobile.action.KEEP_ALIVE";

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

    /**
     * 用户从最近任务列表划掉应用时的保活处理（第二批 §11）。
     *
     * <p>Manifest 已声明 {@code android:stopWithTask="false"}，因此划掉任务不会
     * 触发 onDestroy（服务与 WebSocket/无障碍捕获通道继续存活，避免"杀掉应用后
     * 后台同步失效"）。这里额外做两件幂等动作：
     * <ol>
     *   <li>催一次连接自愈，回收可能已断开的 WebSocket；</li>
     *   <li>尝试重新拉起本前台服务（已在前台运行时为幂等重入；
     *       系统若因后台启动限制拒绝，仅记录日志，绝不让服务崩溃）。</li>
     * </ol>
     */
    @Override
    public void onTaskRemoved(Intent rootIntent) {
        Log.i(TAG, "任务从最近任务移除（stopWithTask=false），执行保活：保持前台服务与连接");
        SyncManager.getInstance().ensureConnected();
        try {
            Intent keepAlive = new Intent(getApplicationContext(),
                    SyncForegroundService.class);
            keepAlive.setAction(ACTION_KEEP_ALIVE);
            ContextCompat.startForegroundService(getApplicationContext(), keepAlive);
            Log.i(TAG, "保活请求已发出：前台服务保持运行，后台同步不受影响");
        } catch (RuntimeException e) {
            // Android 12+ 后台启动前台服务受限：服务本身未被销毁，仅记录即可
            Log.w(TAG, "保活重启前台服务未获系统许可: " + e.getMessage());
        }
        super.onTaskRemoved(rootIntent);
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

package com.cliplink.mobile;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.Toast;

import androidx.core.content.ContextCompat;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.cliplink.mobile.accessibility.AccessibilityGuard;
import com.cliplink.mobile.clipboard.ClipboardSource;
import com.cliplink.mobile.data.ClipboardRepository;
import com.cliplink.mobile.data.ConfigRepository;
import com.cliplink.mobile.protocol.ClipboardEvent;
import com.cliplink.mobile.service.SyncForegroundService;
import com.cliplink.mobile.sync.SyncManager;
import com.cliplink.mobile.sync.WebSocketClientWrapper;
import com.cliplink.mobile.system.BackgroundGuard;

import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * 单主界面（§7 主界面 / §62 UI 与业务分离：本类只做展示与用户操作转发）。
 *
 * - 顶部状态条：连接三态 + 当前服务器地址，点击重连（§38 / §101）
 * - 暂停/恢复按钮：对接 SyncManager.setPaused（§65）
 * - 配置区：服务器地址/端口/设备名保存至 ConfigRepository 并立即重连；
 *   deviceId 只显示末 8 位（§19 / §102）
 * - 历史列表：RecyclerView，点击条目走 HISTORY 路径写剪贴板（§76 / §83）
 * - 生命周期：onResume 重载历史与状态（§63 事件通知刷新，不轮询）
 * - 首次启动前台服务承载同步（§91-§92）；Android 13+ 申请一次通知权限
 */
public class MainActivity extends Activity
        implements SyncManager.StateListener {

    private static final String TAG = "ClipLink";
    private static final int REQ_POST_NOTIFICATIONS = 1001;
    private static final String PREFS_UI = "cliplink_ui";
    private static final String KEY_NOTIFICATION_ASKED = "notificationAsked";

    /** 历史列表单次加载上限 */
    private static final int HISTORY_LIMIT = 200;

    private View statusBar;
    private View statusDot;
    private TextView tvStatusText;
    private TextView tvStatusServer;
    private Button btnPause;
    private EditText etServerAddress;
    private EditText etPort;
    private EditText etDeviceName;
    private TextView tvDeviceId;
    private RecyclerView rvHistory;
    private TextView tvEmpty;

    /** 后台常驻自检提示条（§91：电池优化白名单 / 后台限制未满足时的引导） */
    private View boxSelfCheck;
    private TextView tvSelfCheck;
    private Button btnBatteryWhitelist;
    private Button btnAutoStart;
    private Button btnFreeze;

    /**
     * 自检提示条内的"一键开启无障碍"入口（第二批 §10）。
     *
     * <p>无障碍未启用时随自检提示条一起显示，点击跳系统无障碍设置页。
     */
    private Button btnSelfCheckAccessibility;

    /** 无障碍未启用告警是否已提示过（用于恢复后提示"已恢复"并自动收起） */
    private boolean accessibilitySelfCheckAlerted = false;

    /** 无障碍服务状态条（后台复制/剪切捕获通道的存在性与开关入口） */
    private View boxAccessibility;
    private TextView tvAccessibilityStatus;
    private TextView tvAccessibilityHint;
    private Button btnAccessibilitySettings;

    /** 自检未通过时只在每次进入前台提示一次，避免反复打扰 */
    private boolean selfCheckToastShown = false;

    /** 无障碍未开启时同样只在每次进入前台提示一次 */
    private boolean accessibilityToastShown = false;

    private HistoryAdapter adapter;
    private ConfigRepository config;

    /** 历史查询走后台线程，避免 SQLite 阻塞 UI（§62） */
    private final ExecutorService historyExecutor =
            Executors.newSingleThreadExecutor();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        config = new ConfigRepository(this);
        bindViews();
        setupHistoryList();
        fillConfigFields();

        // UI 状态监听（§63：同步层 -> 通知 UI，回调已在主线程）
        SyncManager.getInstance().setStateListener(this);

        // 应用启动即以前台服务承载同步（Android 不允许永久后台监听，§91）
        ContextCompat.startForegroundService(
                this, new Intent(this, SyncForegroundService.class));

        // Android 13+ 一次性申请 POST_NOTIFICATIONS（前台服务通知可见）
        maybeRequestNotificationPermission();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // 回到前台：立即催一次连接自愈（§91 后台被冻结/切网后恢复）
        SyncManager.getInstance().ensureConnected();
        // 恢复前台时重载历史与状态（§63）
        applyConnectionState(SyncManager.getInstance().getConnectionState());
        applyPausedState(SyncManager.getInstance().isPaused());
        reloadHistory();
        // 后台常驻自检（§91-§92：电池优化白名单 / ColorOS 后台限制）
        refreshSelfCheck();
        // 无障碍服务状态（后台复制/剪切捕获通道，回前台时同步一次）
        refreshAccessibilityState();
    }

    @Override
    protected void onDestroy() {
        SyncManager.getInstance().setStateListener(null);
        historyExecutor.shutdown();
        super.onDestroy();
    }

    // ---- 视图绑定与初始化 ---------------------------------------------------

    private void bindViews() {
        statusBar = findViewById(R.id.bar_status);
        statusDot = findViewById(R.id.view_status_dot);
        tvStatusText = findViewById(R.id.tv_status_text);
        tvStatusServer = findViewById(R.id.tv_status_server);
        btnPause = findViewById(R.id.btn_pause);
        etServerAddress = findViewById(R.id.et_server_address);
        etPort = findViewById(R.id.et_port);
        etDeviceName = findViewById(R.id.et_device_name);
        tvDeviceId = findViewById(R.id.tv_device_id);
        rvHistory = findViewById(R.id.rv_history);
        tvEmpty = findViewById(R.id.tv_empty);

        // 点击状态条立即重连（§38）
        statusBar.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                SyncManager.getInstance().reconnect();
                Toast.makeText(MainActivity.this,
                        R.string.status_reconnect_hint,
                        Toast.LENGTH_SHORT).show();
            }
        });

        // 暂停/恢复同步（§65）
        btnPause.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                boolean paused = SyncManager.getInstance().isPaused();
                SyncManager.getInstance().setPaused(!paused);
            }
        });

        // 保存配置并立即生效重连（§101 禁止写死地址）
        findViewById(R.id.btn_save_config).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        saveConfig();
                    }
                });

        // 后台常驻自检提示条（默认隐藏，由 refreshSelfCheck 决定是否展示）
        boxSelfCheck = findViewById(R.id.box_self_check);
        tvSelfCheck = findViewById(R.id.tv_self_check);
        btnBatteryWhitelist = findViewById(R.id.btn_battery_whitelist);
        btnAutoStart = findViewById(R.id.btn_autostart_settings);
        btnFreeze = findViewById(R.id.btn_freeze_settings);
        btnSelfCheckAccessibility =
                findViewById(R.id.btn_self_check_accessibility);
        boxSelfCheck.setVisibility(View.GONE);
        btnSelfCheckAccessibility.setVisibility(View.GONE);

        // ④ 自检提示条内的无障碍一键入口（第二批 §10）：
        //    直接跳系统无障碍设置页，失败退本应用详情页
        btnSelfCheckAccessibility.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!AccessibilityGuard
                        .openAccessibilitySettings(MainActivity.this)) {
                    fallbackToAppDetails();
                    Toast.makeText(MainActivity.this,
                            R.string.accessibility_jump_failed,
                            Toast.LENGTH_LONG).show();
                } else {
                    Toast.makeText(MainActivity.this,
                            R.string.accessibility_jump_hint,
                            Toast.LENGTH_LONG).show();
                }
            }
        });

        // ① 申请电池优化白名单（系统对话框；不可用则退系统电池优化列表）
        btnBatteryWhitelist.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                boolean ok = BackgroundGuard
                        .requestIgnoreBatteryOptimizations(MainActivity.this);
                if (!ok) {
                    ok = BackgroundGuard
                            .openBatteryOptimizationList(MainActivity.this);
                }
                if (!ok) {
                    fallbackToAppDetails();
                }
            }
        });

        // ② 跳转 ColorOS 自启动管理，失败退本应用详情页
        btnAutoStart.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!BackgroundGuard.openAutoStartSettings(MainActivity.this)) {
                    fallbackToAppDetails();
                }
            }
        });

        // ③ 跳转 ColorOS 应用速冻 / 省电设置，失败退本应用详情页
        btnFreeze.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!BackgroundGuard.openFreezeSettings(MainActivity.this)) {
                    fallbackToAppDetails();
                }
            }
        });

        // ④ 无障碍服务：后台复制/剪切捕获通道的状态展示与一键开启入口
        boxAccessibility = findViewById(R.id.box_accessibility);
        tvAccessibilityStatus = findViewById(R.id.tv_accessibility_status);
        tvAccessibilityHint = findViewById(R.id.tv_accessibility_hint);
        btnAccessibilitySettings =
                findViewById(R.id.btn_accessibility_settings);
        btnAccessibilitySettings.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (AccessibilityGuard.isServiceEnabled(MainActivity.this)) {
                    Toast.makeText(MainActivity.this,
                            R.string.accessibility_jump_hint,
                            Toast.LENGTH_SHORT).show();
                }
                // 跳转系统无障碍设置页，失败退本应用详情页（同自检引导兜底）
                if (!AccessibilityGuard
                        .openAccessibilitySettings(MainActivity.this)) {
                    fallbackToAppDetails();
                    Toast.makeText(MainActivity.this,
                            R.string.accessibility_jump_failed,
                            Toast.LENGTH_LONG).show();
                }
            }
        });
    }

    // ---- 后台常驻自检（§91-§92）----------------------------------------------

    /**
     * 后台常驻自检（§91-§92 + 第二批 §10）：
     * 检查电池优化白名单 / 后台限制 / 无障碍服务启用状态，
     * 未满足时展示提示条并引导设置；全部满足则自动收起提示条。
     *
     * <p>无障碍检测说明（第二批 §10/§12）：应用无法自行开启无障碍服务，
     * 因此这里只做"检测 + 引导 + 复检"：每次回到前台（onResume）都会重新检测，
     * 用户在系统设置里开启后返回应用，提示条会自动收起（无需重启应用）。
     */
    private void refreshSelfCheck() {
        BackgroundGuard.Status status = BackgroundGuard.check(this);
        boolean accessibilityEnabled = AccessibilityGuard.isServiceEnabled(this);

        if (status.satisfied() && accessibilityEnabled) {
            // 全部满足：自动收起提示条（含无障碍开启后自动收起）
            boxSelfCheck.setVisibility(View.GONE);
            btnSelfCheckAccessibility.setVisibility(View.GONE);
            selfCheckToastShown = false;
            if (accessibilitySelfCheckAlerted) {
                accessibilitySelfCheckAlerted = false;
                Toast.makeText(this, R.string.self_check_accessibility_restored,
                        Toast.LENGTH_SHORT).show();
            }
            return;
        }

        StringBuilder sb = new StringBuilder();
        sb.append(getString(R.string.self_check_title));
        if (!status.ignoringBatteryOptimizations) {
            sb.append('\n').append(getString(R.string.self_check_battery_missing));
        }
        if (status.backgroundRestricted) {
            sb.append('\n').append(getString(R.string.self_check_restricted));
        }
        if (!accessibilityEnabled) {
            // 后台同步会失效：如实说明 + 引导用户手动开启（§12）
            sb.append('\n').append(
                    getString(R.string.self_check_accessibility_missing));
            sb.append('\n').append(
                    getString(R.string.self_check_accessibility_hint));
            accessibilitySelfCheckAlerted = true;
        }
        sb.append('\n').append(getString(R.string.self_check_hint));
        tvSelfCheck.setText(sb.toString());
        boxSelfCheck.setVisibility(View.VISIBLE);
        // 无障碍未启用时才显示"一键开启无障碍服务"入口
        btnSelfCheckAccessibility.setVisibility(accessibilityEnabled
                ? View.GONE : View.VISIBLE);

        if (!selfCheckToastShown) {
            selfCheckToastShown = true;
            Toast.makeText(this, R.string.self_check_toast,
                    Toast.LENGTH_LONG).show();
        }
    }

    /** 跳转失败时的优雅兜底：打开本应用详情页（所有 ColorOS 版本均可用） */
    private void fallbackToAppDetails() {
        BackgroundGuard.openAppDetails(this);
        Toast.makeText(this, R.string.self_check_jump_failed,
                Toast.LENGTH_LONG).show();
    }

    // ---- 无障碍服务状态（后台复制/剪切捕获通道）------------------------------

    /**
     * 无障碍服务开启状态展示（见全局任务 4）。
     *
     * 已开启：展示运行中状态与去重说明，按钮变为"管理无障碍服务"；
     * 未开启：展示未开启状态与影响提示，按钮为"开启无障碍服务"，
     *         并在每次进入前台时提示一次（不反复打扰，与自检提示同策略）。
     */
    private void refreshAccessibilityState() {
        boolean enabled = AccessibilityGuard.isServiceEnabled(this);
        if (tvAccessibilityStatus == null) {
            return;
        }
        tvAccessibilityStatus.setText(enabled
                ? R.string.accessibility_status_on
                : R.string.accessibility_status_off);
        tvAccessibilityHint.setText(enabled
                ? R.string.accessibility_hint_on
                : R.string.accessibility_hint_off);
        btnAccessibilitySettings.setText(enabled
                ? R.string.btn_accessibility_settings_on
                : R.string.btn_accessibility_settings_off);
        boxAccessibility.setVisibility(View.VISIBLE);

        if (enabled) {
            accessibilityToastShown = false;
            return;
        }
        if (!accessibilityToastShown) {
            accessibilityToastShown = true;
            Toast.makeText(this, R.string.accessibility_toast_off,
                    Toast.LENGTH_LONG).show();
        }
    }

    private void setupHistoryList() {
        adapter = new HistoryAdapter(new HistoryAdapter.OnItemClickListener() {
            @Override
            public void onItemClick(ClipboardEvent event) {
                // §76 / §83：点击历史 -> HISTORY 路径写剪贴板，
                // 不入库、不发送 WebSocket（由 SyncManager 路由保证）
                SyncManager.getInstance().handleClipboardEvent(
                        ClipboardSource.HISTORY, event.getContent());
                Toast.makeText(MainActivity.this,
                        R.string.toast_history_copied,
                        Toast.LENGTH_SHORT).show();
            }
        });
        rvHistory.setLayoutManager(new LinearLayoutManager(this));
        rvHistory.setAdapter(adapter);
    }

    private void fillConfigFields() {
        etServerAddress.setText(config.getServerAddress());
        etPort.setText(String.valueOf(config.getPort()));
        etDeviceName.setText(config.getDeviceName());
        // deviceId 只显示末 8 位（§19 持久化，仅作展示）
        String deviceId = config.getDeviceId();
        String shortId = deviceId.length() > 8
                ? deviceId.substring(deviceId.length() - 8) : deviceId;
        tvDeviceId.setText(getString(R.string.label_device_id) + ": " + shortId);
    }

    // ---- 配置保存 -----------------------------------------------------------

    private void saveConfig() {
        String address = etServerAddress.getText().toString().trim();
        String portText = etPort.getText().toString().trim();
        String deviceName = etDeviceName.getText().toString().trim();

        if (address.isEmpty()) {
            Toast.makeText(this, R.string.toast_address_empty,
                    Toast.LENGTH_SHORT).show();
            return;
        }
        int port;
        try {
            port = Integer.parseInt(portText);
        } catch (NumberFormatException e) {
            port = -1;
        }
        if (port < 1 || port > 65535) {
            Toast.makeText(this, R.string.toast_port_invalid,
                    Toast.LENGTH_SHORT).show();
            return;
        }

        config.setServerAddress(address);
        config.setPort(port);
        if (!deviceName.isEmpty()) {
            config.setDeviceName(deviceName);
        }
        // 立即生效：按最新配置重建 WebSocket（§38 / §101）
        SyncManager.getInstance().reconnect();
        applyConnectionState(SyncManager.getInstance().getConnectionState());
        tvStatusServer.setText(getString(R.string.status_server,
                config.getServerUrl()));
        Toast.makeText(this, R.string.toast_config_saved,
                Toast.LENGTH_SHORT).show();
    }

    // ---- 状态刷新 -----------------------------------------------------------

    /** 连接三态 -> 状态条文案与配色（§38） */
    private void applyConnectionState(WebSocketClientWrapper.State state) {
        int textRes;
        int colorRes;
        switch (state) {
            case CONNECTED:
                textRes = R.string.status_connected;
                colorRes = R.color.status_connected;
                break;
            case CONNECTING:
                textRes = R.string.status_connecting;
                colorRes = R.color.status_connecting;
                break;
            case DISCONNECTED:
            default:
                textRes = R.string.status_disconnected;
                colorRes = R.color.status_disconnected;
                break;
        }
        tvStatusText.setText(textRes);
        tvStatusText.setTextColor(ContextCompat.getColor(this, colorRes));
        statusDot.setBackgroundColor(ContextCompat.getColor(this, colorRes));
        tvStatusServer.setText(getString(R.string.status_server,
                config.getServerUrl()));
    }

    /** 暂停/恢复按钮状态（§65：必须明确展示 同步开启/暂停） */
    private void applyPausedState(boolean paused) {
        btnPause.setText(paused
                ? R.string.btn_resume_sync : R.string.btn_pause_sync);
    }

    // ---- SyncManager.StateListener（回调已在主线程）------------------------

    @Override
    public void onConnectionStateChanged(WebSocketClientWrapper.State state) {
        applyConnectionState(state);
    }

    @Override
    public void onSyncPausedChanged(boolean paused) {
        applyPausedState(paused);
        Toast.makeText(this, paused
                        ? R.string.toast_sync_paused
                        : R.string.toast_sync_resumed,
                Toast.LENGTH_SHORT).show();
    }

    @Override
    public void onHistoryChanged() {
        // 历史入库/ACK 变化 -> 刷新列表（§63，禁止 UI 轮询数据库）
        reloadHistory();
    }

    // ---- 历史加载 -----------------------------------------------------------

    private void reloadHistory() {
        final ClipboardRepository repo =
                ClipLinkApp.getInstance().getClipboardRepository();
        if (repo == null) {
            return;
        }
        historyExecutor.execute(new Runnable() {
            @Override
            public void run() {
                final List<ClipboardEvent> data =
                        repo.queryHistory(0, HISTORY_LIMIT);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (isFinishing() || isDestroyed()) {
                            return;
                        }
                        adapter.submitList(data);
                        tvEmpty.setVisibility(data.isEmpty()
                                ? View.VISIBLE : View.GONE);
                        rvHistory.setVisibility(data.isEmpty()
                                ? View.INVISIBLE : View.VISIBLE);
                    }
                });
            }
        });
    }

    // ---- 通知权限（Android 13+）--------------------------------------------

    private void maybeRequestNotificationPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            return;
        }
        if (ContextCompat.checkSelfPermission(this,
                Manifest.permission.POST_NOTIFICATIONS)
                == PackageManager.PERMISSION_GRANTED) {
            return;
        }
        SharedPreferences prefs =
                getSharedPreferences(PREFS_UI, MODE_PRIVATE);
        if (prefs.getBoolean(KEY_NOTIFICATION_ASKED, false)) {
            return; // 只申请一次
        }
        prefs.edit().putBoolean(KEY_NOTIFICATION_ASKED, true).apply();
        requestPermissions(
                new String[]{Manifest.permission.POST_NOTIFICATIONS},
                REQ_POST_NOTIFICATIONS);
    }
}

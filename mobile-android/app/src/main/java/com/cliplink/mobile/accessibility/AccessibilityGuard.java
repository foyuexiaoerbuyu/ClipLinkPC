package com.cliplink.mobile.accessibility;

import android.accessibilityservice.AccessibilityServiceInfo;
import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.provider.Settings;
import android.text.TextUtils;
import android.util.Log;
import android.view.accessibility.AccessibilityManager;

import java.util.List;

/**
 * 无障碍服务状态检查与系统设置引导（后台复制/剪切通道可用性判定）。
 *
 * <p>与 {@code BackgroundGuard} 同风格：只读检查 + 跳转引导，
 * 任何异常都按"不可用/未开启"返回并记录日志，绝不让界面崩溃（§72）。
 */
public final class AccessibilityGuard {

    private static final String TAG = "ClipLink";

    private AccessibilityGuard() {
    }

    /**
     * ClipLink 无障碍服务当前是否已开启。
     *
     * <p>① 以 AccessibilityManager 的"系统当前生效服务列表"为准（最权威）；
     * ② 列表读不到时，退回解析 Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES。
     */
    public static boolean isServiceEnabled(Context context) {
        Context app = context.getApplicationContext();
        String packageName = app.getPackageName();
        String className = ClipLinkAccessibilityService.class.getName();

        try {
            AccessibilityManager manager = (AccessibilityManager)
                    app.getSystemService(Context.ACCESSIBILITY_SERVICE);
            if (manager != null) {
                List<AccessibilityServiceInfo> services =
                        manager.getEnabledAccessibilityServiceList(
                                AccessibilityServiceInfo.FEEDBACK_ALL_MASK);
                if (services != null) {
                    for (AccessibilityServiceInfo info : services) {
                        if (info == null || info.getResolveInfo() == null
                                || info.getResolveInfo().serviceInfo == null) {
                            continue;
                        }
                        if (packageName.equals(
                                info.getResolveInfo().serviceInfo.packageName)
                                && className.equals(
                                        info.getResolveInfo().serviceInfo.name)) {
                            return true;
                        }
                    }
                }
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "读取无障碍服务列表失败: " + e.getMessage());
        }
        return isEnabledInSecureSettings(app, packageName, className);
    }

    /** 解析系统设置中已启用的无障碍服务列表（读取失败按未开启处理） */
    private static boolean isEnabledInSecureSettings(Context app,
                                                     String packageName,
                                                     String className) {
        try {
            String enabled = Settings.Secure.getString(app.getContentResolver(),
                    Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES);
            if (TextUtils.isEmpty(enabled)) {
                return false;
            }
            TextUtils.SimpleStringSplitter splitter =
                    new TextUtils.SimpleStringSplitter(':');
            splitter.setString(enabled);
            while (splitter.hasNext()) {
                ComponentName component =
                        ComponentName.unflattenFromString(splitter.next());
                if (component != null
                        && packageName.equals(component.getPackageName())
                        && className.equals(component.getClassName())) {
                    return true;
                }
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "解析无障碍服务设置失败: " + e.getMessage());
        }
        return false;
    }

    /**
     * 打开系统无障碍设置页（一键入口）。
     *
     * @return true 表示设置页已成功拉起；false 由调用方给出兜底提示
     */
    public static boolean openAccessibilitySettings(Activity activity) {
        try {
            Intent intent = new Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            if (activity.getPackageManager().resolveActivity(intent, 0) == null) {
                Log.w(TAG, "系统无障碍设置页不存在");
                return false;
            }
            activity.startActivity(intent);
            Log.i(TAG, "已打开系统无障碍设置页");
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "无障碍设置页不可用: " + e.getMessage());
            return false;
        }
    }
}

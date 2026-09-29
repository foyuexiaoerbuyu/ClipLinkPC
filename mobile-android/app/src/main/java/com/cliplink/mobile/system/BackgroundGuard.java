package com.cliplink.mobile.system;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.Log;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * 后台常驻自检与系统设置引导（见需求 §91-§92 后台常驻前提）。
 *
 * <p>职责：
 * <ul>
 *   <li>{@link #check(Context)} 判断本应用是否已加入电池优化白名单、
 *       是否被系统限制后台运行（ColorOS 的"应用速冻/后台冻结"会体现为
 *       {@code ActivityManager.isBackgroundRestricted()} 为 true）；</li>
 *   <li>{@link #openAutoStartSettings(Activity)} 跳转 ColorOS 自启动管理；</li>
 *   <li>{@link #openFreezeSettings(Activity)} 跳转 ColorOS 应用速冻 / 电池省电设置；</li>
 *   <li>{@link #requestIgnoreBatteryOptimizations(Activity)} 弹系统对话框申请电池优化白名单；</li>
 *   <li>各跳转方法逐候选组件尝试，全部失败时返回 false，
 *       由调用方统一退回 {@link #openAppDetails(Activity)}（本应用详情页）。</li>
 * </ul>
 *
 * <p>注意：ColorOS 各版本的自启动/速冻页面组件名并不统一（部分机型页面还由
 * 插件包动态提供），因此这里维护"候选组件列表 + 系统标准 Intent 兜底"，
 * 逐个尝试并在失败时优雅退化，绝不因为跳转失败而崩溃（§72）。
 */
public final class BackgroundGuard {

    private static final String TAG = "ClipLink";

    /** ColorOS / ColorOS 衍生（realme / OPPO）手机管家包名候选 */
    private static final String[] SAFE_CENTER_PKGS = {
            "com.oplus.safecenter",   // ColorOS 12+ (OPPO/一加)
            "com.coloros.safecenter", // ColorOS 6~11
            "com.oppo.safe"           // ColorOS 3~5
    };

    /** 自启动管理页候选组件（"包名/类名"形式） */
    private static final String[] AUTO_START_COMPONENTS = {
            "com.oplus.safecenter/com.oplus.safecenter.startupapp.StartupAppListActivity",
            "com.oplus.safecenter/com.oplus.safecenter.permission.startup.StartupAppListActivity",
            "com.oplus.safecenter/com.oplus.safecenter.startup.StartupAppListActivity",
            "com.coloros.safecenter/com.coloros.safecenter.startupapp.StartupAppListActivity",
            "com.coloros.safecenter/com.coloros.safecenter.permission.startup.StartupAppListActivity",
            "com.coloros.safecenter/com.coloros.safecenter.startup.StartupAppListActivity",
            "com.oppo.safe/com.oppo.safe.permission.startup.StartupAppListActivity",
            "com.oppo.safe/com.oppo.safe.startupapp.StartupAppListActivity"
    };

    /** 应用速冻 / 省电（后台冻结）页候选组件 */
    private static final String[] FREEZE_COMPONENTS = {
            "com.oplus.safecenter/com.oplus.safecenter.freeze.FreezeAppListActivity",
            "com.oplus.safecenter/com.oplus.safecenter.powermanager.PowerManagerActivity",
            "com.oplus.safecenter/com.oplus.safecenter.startup.FreezeAppListActivity",
            "com.coloros.safecenter/com.coloros.safecenter.freeze.FreezeAppListActivity",
            "com.coloros.safecenter/com.coloros.safecenter.powermanager.PowerManagerActivity",
            "com.oplus.battery/com.oplus.battery.ui.BatteryActivity",
            "com.coloros.oppoguardelf/com.coloros.powermanager.fuelgaue.PowerUsageModelActivity"
    };

    /** 自启动管理可能的 action（部分 ColorOS 版本以 action 暴露，不导出组件） */
    private static final String[] AUTO_START_ACTIONS = {
            "com.oplus.safecenter.action.STARTUP_APP_LIST",
            "com.coloros.safecenter.action.STARTUP_APP_LIST",
            "oppo.intent.action.STARTUP_APP_LIST"
    };

    private BackgroundGuard() {
    }

    /** 自检结果快照 */
    public static final class Status {
        /** 是否已加入电池优化白名单（Doze 豁免） */
        public final boolean ignoringBatteryOptimizations;
        /** 是否被系统限制后台运行（ColorOS 应用速冻/后台冻结会置为 true） */
        public final boolean backgroundRestricted;

        Status(boolean ignoringBatteryOptimizations, boolean backgroundRestricted) {
            this.ignoringBatteryOptimizations = ignoringBatteryOptimizations;
            this.backgroundRestricted = backgroundRestricted;
        }

        /** 后台常驻前提是否已全部满足 */
        public boolean satisfied() {
            return ignoringBatteryOptimizations && !backgroundRestricted;
        }
    }

    /** 检查电池优化白名单与后台限制状态；任何异常都按"未满足"返回（不崩溃，§72） */
    public static Status check(Context context) {
        boolean ignoring = checkIgnoringBatteryOptimizations(context);
        boolean restricted = checkBackgroundRestricted(context);
        Log.i(TAG, "后台自检: 电池优化白名单=" + ignoring
                + ", 后台运行受限=" + restricted
                + (ignoring && !restricted ? " -> 已满足" : " -> 需要引导用户设置"));
        return new Status(ignoring, restricted);
    }

    private static boolean checkIgnoringBatteryOptimizations(Context context) {
        try {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
                return true; // Android 6 以下无电池优化机制
            }
            PowerManager pm = (PowerManager)
                    context.getSystemService(Context.POWER_SERVICE);
            return pm != null
                    && pm.isIgnoringBatteryOptimizations(context.getPackageName());
        } catch (RuntimeException e) {
            Log.w(TAG, "读取电池优化白名单失败: " + e.getMessage());
            return false;
        }
    }

    private static boolean checkBackgroundRestricted(Context context) {
        try {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
                return false; // Android 9 以下无该接口
            }
            ActivityManager am = (ActivityManager)
                    context.getSystemService(Context.ACTIVITY_SERVICE);
            return am != null && am.isBackgroundRestricted();
        } catch (RuntimeException e) {
            Log.w(TAG, "读取后台限制状态失败: " + e.getMessage());
            return false;
        }
    }

    // ---- 跳转引导 -----------------------------------------------------------

    /**
     * 弹出系统"忽略电池优化"授权对话框（需要 REQUEST_IGNORE_BATTERY_OPTIMIZATIONS 权限）。
     *
     * @return true 表示对话框已成功拉起
     */
    public static boolean requestIgnoreBatteryOptimizations(Activity activity) {
        try {
            Intent intent = new Intent(
                    Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS);
            intent.setData(Uri.parse("package:" + activity.getPackageName()));
            activity.startActivity(intent);
            Log.i(TAG, "已拉起电池优化白名单授权对话框");
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "电池优化授权对话框不可用: " + e.getMessage());
            return false;
        }
    }

    /** 打开系统电池优化设置列表（部分机型即"应用速冻/省电"入口） */
    public static boolean openBatteryOptimizationList(Activity activity) {
        try {
            activity.startActivity(new Intent(
                    Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS));
            Log.i(TAG, "已打开系统电池优化设置列表");
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "电池优化设置列表不可用: " + e.getMessage());
            return false;
        }
    }

    /** 打开 ColorOS 自启动管理页；候选全部失败返回 false */
    public static boolean openAutoStartSettings(Activity activity) {
        List<Intent> candidates = new ArrayList<>();
        for (String component : AUTO_START_COMPONENTS) {
            candidates.add(componentIntent(activity, component));
        }
        for (String action : AUTO_START_ACTIONS) {
            Intent intent = new Intent(action);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            candidates.add(intent);
        }
        return startFirstAvailable(activity, candidates, "自启动管理");
    }

    /** 打开 ColorOS 应用速冻 / 省电后台管理页；候选全部失败返回 false */
    public static boolean openFreezeSettings(Activity activity) {
        List<Intent> candidates = new ArrayList<>();
        for (String component : FREEZE_COMPONENTS) {
            candidates.add(componentIntent(activity, component));
        }
        // ColorOS 部分版本把"应用速冻"并入系统电池优化列表
        Intent batteryList = new Intent(
                Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS);
        candidates.add(batteryList);
        return startFirstAvailable(activity, candidates, "应用速冻设置");
    }

    /** 打开本应用详情页（最终兜底：所有机型都有，且能手动进自启动/省电设置） */
    public static boolean openAppDetails(Activity activity) {
        try {
            Intent intent = new Intent(
                    Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
            intent.setData(Uri.parse("package:" + activity.getPackageName()));
            activity.startActivity(intent);
            Log.i(TAG, "已打开本应用详情页（引导兜底）");
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "应用详情页不可用: " + e.getMessage());
            return false;
        }
    }

    // ---- 内部工具 -----------------------------------------------------------

    private static Intent componentIntent(Activity activity, String component) {
        Intent intent = new Intent();
        intent.setComponent(ComponentName.unflattenFromString(component));
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        return intent;
    }

    /**
     * 依次尝试候选 Intent，第一个可成功拉起的即返回 true。
     * 组件不存在 / 未导出（SecurityException）都视为该候选不可用。
     */
    private static boolean startFirstAvailable(Activity activity,
                                               List<Intent> candidates,
                                               String target) {
        for (Intent intent : candidates) {
            if (intent.getComponent() == null && intent.getAction() == null) {
                continue;
            }
            if (intent.getComponent() != null
                    && !isComponentAvailable(activity, intent)) {
                continue; // 先做可见性/存在性校验，避免无谓的 ActivityNotFound
            }
            try {
                activity.startActivity(intent);
                Log.i(TAG, "已打开" + target + "页: " + describe(intent));
                return true;
            } catch (RuntimeException e) {
                Log.w(TAG, "打开" + target + "页失败(" + describe(intent)
                        + "): " + e.getMessage());
            }
        }
        Log.w(TAG, target + "页全部候选不可用，候选数=" + candidates.size());
        return false;
    }

    /** 组件是否在系统中真实存在（避免直接 startActivity 抛 ActivityNotFound） */
    private static boolean isComponentAvailable(Activity activity, Intent intent) {
        try {
            return activity.getPackageManager()
                    .resolveActivity(intent, 0) != null;
        } catch (RuntimeException e) {
            return false;
        }
    }

    private static String describe(Intent intent) {
        if (intent.getComponent() != null) {
            return intent.getComponent().flattenToShortString();
        }
        return "action=" + intent.getAction();
    }

    /** 供日志/诊断输出：手机管家类包名列表（只读） */
    public static List<String> safeCenterPackages() {
        return Arrays.asList(SAFE_CENTER_PKGS);
    }
}

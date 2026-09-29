// ---------------------------------------------------------------------------
// ClipLink Windows MVP - 应用入口（总装，需求 §61 / §62 / §113-§115）
// 职责：初始化基础设施（日志 / 配置 / 设备标识 / 数据库）、
//       创建隐藏宿主窗口、装配 UI / 托盘 / 开机启动 / 剪贴板 / 同步，
//       进入 Win32 消息循环。UI 与业务层通过回调/接口解耦（§62）：
//       - Repository 变更 -> UiDispatcher 广播 -> 主窗口刷新（§63，非轮询）
//       - Sync 状态变化   -> UiDispatcher 广播 -> 主窗口/托盘刷新（§38）
//       - UI 操作         -> 业务层公开接口（SyncManager / ConfigManager /
//                            ClipboardMonitor / StartupManager）
// ---------------------------------------------------------------------------

#include <windows.h>
#include <shellapi.h>

#include "clipboard/ClipboardMonitor.h"
#include "common/DataTypes.h"
#include "config/ConfigManager.h"
#include "database/ClipboardRepository.h"
#include "database/DatabaseManager.h"
#include "database/StateRepository.h"
#include "device/DeviceManager.h"
#include "sync/SyncManager.h"
#include "sync/WebSocketClient.h"
#include "system/StartupManager.h"
#include "tray/TrayIcon.h"
#include "ui/MainWindow.h"
#include "ui/SettingsWindow.h"
#include "ui/UiDispatcher.h"
#include "util/Logger.h"

using namespace cliplink;
using cliplink::util::Logger;

namespace {

constexpr wchar_t kHiddenWindowClass[] = L"ClipLinkHiddenWindow";
constexpr wchar_t kHiddenWindowText[]   = L"ClipLink";

HWND g_hiddenWindow = nullptr;

// 隐藏宿主窗口：承载 AddClipboardFormatListener 的 WM_CLIPBOARDUPDATE 消息
// （§11 / §103 事件监听，非轮询）。退出统一由托盘"退出"经此窗口驱动。
LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                               LPARAM lParam) {
    switch (msg) {
        case WM_CLIPBOARDUPDATE:
            // §81 事件主流程入口：读取 -> SHA-256 -> 防循环 ->
            // eventId -> 写本地历史 -> 回调通知 Sync 层
            clipboard::ClipboardMonitor::instance().onClipboardUpdate();
            return 0;
        case WM_CLOSE:
            // §64：主窗口关闭为隐藏到托盘；退出仅由托盘菜单触发，
            // 托盘"退出"直接 DestroyWindow 此宿主窗口走 WM_DESTROY。
            return 0;
        case WM_DESTROY:
            // §11：程序退出时移除剪贴板监听
            clipboard::ClipboardMonitor::instance().detach();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool createHiddenWindow(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &HiddenWndProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kHiddenWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (wc.hIcon == nullptr) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CLIPLOG_ERROR("main", "隐藏窗口类注册失败, errno=" +
                                  std::to_string(GetLastError()));
        return false;
    }

    // WS_POPUP + 不显示：无任务栏图标、无可见窗口，纯消息宿主
    g_hiddenWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW, kHiddenWindowClass, kHiddenWindowText, WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, instance, nullptr);

    if (g_hiddenWindow == nullptr) {
        CLIPLOG_ERROR("main", "隐藏窗口创建失败, errno=" +
                                  std::to_string(GetLastError()));
        return false;
    }
    return true;
}

// 托盘"退出"：销毁宿主窗口 -> WM_DESTROY -> 剪贴板监听注销 + 退出消息循环
void exitApplication() {
    if (g_hiddenWindow != nullptr) {
        DestroyWindow(g_hiddenWindow);
    }
}

// 装配 UI 层（§4 默认后台托盘运行，不弹主窗口）
bool assembleUi(HINSTANCE instance) {
    // 1) Repository 变更 -> 广播刷新（§63 事件驱动，禁止 UI 轮询 SELECT）
    database::ClipboardRepository::instance().setChangeCallback(
        []() { ui::UiDispatcher::instance().broadcast(ui::kMsgHistoryChanged); });

    // 2) 同步状态变化 -> 广播刷新（回调来自 WebSocket 线程，
    //    broadcast 内部 PostMessage 回 UI 线程，§56 线程模型）
    sync::SyncManager::instance().setStateCallback(
        [](sync::WebSocketClient&) {
            ui::UiDispatcher::instance().broadcast(ui::kMsgConnState);
        });

    // 3) 主窗口：创建但保持隐藏（§4 默认不弹出）
    if (!ui::MainWindow::instance().create(instance)) {
        CLIPLOG_ERROR("main", "主窗口创建失败");
        return false;
    }

    // 4) 设置窗口：预创建保持隐藏；⚙ 回调解耦装配（§7 / §62）
    if (!ui::SettingsWindow::instance().create(instance)) {
        CLIPLOG_ERROR("main", "设置窗口创建失败");
        return false;
    }
    ui::MainWindow::instance().setSettingsHandler([]() {
        ui::SettingsWindow::instance().show();
    });

    // 5) 托盘图标（§4 / §66）：左键/双击开主窗、菜单驱动业务
    tray::TrayIcon::Handlers handlers;
    handlers.openMain = []() { ui::MainWindow::instance().show(); };
    handlers.openSettings = []() { ui::SettingsWindow::instance().show(); };
    handlers.onExit = &exitApplication;
    if (!tray::TrayIcon::instance().create(instance, std::move(handlers))) {
        CLIPLOG_ERROR("main", "托盘图标创建失败，降级为显示主窗口");
        ui::MainWindow::instance().show();
    }
    return true;
}

}  // namespace

// 供外部模块获取宿主窗口句柄（ClipboardMonitor 的 AddClipboardFormatListener）
extern "C" HWND ClipLinkHiddenWindowHandle() {
    return g_hiddenWindow;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE /*prevInstance*/,
                   LPSTR /*cmdLine*/, int /*showCmd*/) {
    // 允许系统 DPI 缩放（逻辑设计尺寸 200x500，见需求 §6）
    SetProcessDPIAware();

    // 1. 日志：%APPDATA%\ClipLink\logs\cliplink.log，5MB 滚动
    util::Logger::instance().init();
    CLIPLOG_INFO("main", "ClipLink 启动");

    // 2. 配置：%APPDATA%\ClipLink\config.json（不存在则按默认值创建）
    if (!ConfigManager::instance().load()) {
        CLIPLOG_WARN("main", "配置加载异常，已使用默认配置");
    }

    // 3. 设备标识：首次启动生成 deviceId 并持久化，设备名默认取计算机名
    if (!DeviceManager::instance().initialize()) {
        CLIPLOG_ERROR("main", "设备标识初始化失败");
    } else {
        CLIPLOG_INFO("main", "deviceId=" + DeviceManager::instance().deviceId() +
                                 ", deviceName=" +
                                 DeviceManager::instance().deviceName());
    }

    // 4. 数据库：%APPDATA%\ClipLink\cliplink.db，建表与索引（§24-§26）。
    //    打不开时按降级链处理，不阻断启动（§72）
    if (!database::DatabaseManager::instance().open()) {
        CLIPLOG_ERROR("main", "数据库不可用，历史记录与同步状态暂存受限");
    } else {
        CLIPLOG_INFO("main",
                     "历史记录条数=" +
                         std::to_string(
                             database::ClipboardRepository::instance().count()) +
                         ", lastServerSeq=" +
                         std::to_string(
                             database::StateRepository::instance()
                                 .getLastServerSeq()));
    }

    // 5. 隐藏宿主窗口
    if (!createHiddenWindow(instance)) {
        MessageBoxW(nullptr,
                    L"ClipLink 初始化失败：无法创建宿主窗口。\r\n"
                    L"详情见 %APPDATA%\\ClipLink\\logs\\cliplink.log",
                    L"ClipLink", MB_OK | MB_ICONERROR);
        database::DatabaseManager::instance().close();
        util::Logger::instance().shutdown();
        return 1;
    }

    // 6. 剪贴板监听：AddClipboardFormatListener 注册到宿主窗口（§11 / §103）。
    //    事件主流程见 ClipboardMonitor::onClipboardUpdate（§16 / §81）
    if (!clipboard::ClipboardMonitor::instance().attach(g_hiddenWindow)) {
        CLIPLOG_ERROR("main", "剪贴板监听注册失败，同步与历史功能不可用");
    }

    // 7. UI / 托盘 / 通知广播总装（§61 / §62）
    if (!assembleUi(instance)) {
        CLIPLOG_ERROR("main", "UI 总装失败，功能可能不完整（§72 不退出）");
    }

    // 8. 开机自动启动（§5：HKCU Run 键，仅当前用户、无需管理员、可关闭）。
    //    以 config.autoStart 为准保持注册表一致，失败仅记录不阻断
    const AppConfig bootCfg = ConfigManager::instance().get();
    if (!system::StartupManager::instance().setEnabled(bootCfg.autoStart)) {
        CLIPLOG_WARN("main", "开机启动项同步失败（autoStart=" +
                                 std::string(bootCfg.autoStart ? "1" : "0") +
                                 "）");
    }

    // 9. 同步层：内部注册 USER 事件回调（上行）、启动本机 Server（§67），
    //    并以 WebSocket 客户端连接 config.serverAddress（§39 自动重连）。
    //    失败仅记录日志，不阻断主流程（§72）
    sync::SyncManager::instance().start();

    CLIPLOG_INFO("main", "初始化完成，进入消息循环");

    // 10. 消息循环：无剪贴板事件时线程阻塞于 GetMessage，
    //     保证空闲 CPU 接近 0%（见需求 §2 / §104）
    MSG msg = {};
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (ret == -1) {
        CLIPLOG_ERROR("main", "GetMessage 失败, errno=" +
                                  std::to_string(GetLastError()));
    }

    CLIPLOG_INFO("main", "ClipLink 退出");
    // 逆序释放：托盘 -> UI -> 同步层 -> 数据库 -> 日志
    tray::TrayIcon::instance().destroy();
    ui::SettingsWindow::instance().destroy();
    ui::MainWindow::instance().destroy();
    sync::SyncManager::instance().stop();
    database::DatabaseManager::instance().close();
    util::Logger::instance().shutdown();
    return 0;
}

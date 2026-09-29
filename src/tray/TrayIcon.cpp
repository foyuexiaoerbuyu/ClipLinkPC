// ---------------------------------------------------------------------------
// 托盘图标实现（§4 托盘常驻 / §66 菜单 / §38 Tooltip 状态）
// ---------------------------------------------------------------------------

#include "tray/TrayIcon.h"

#include <cwchar>
#include <iterator>

#include "config/ConfigManager.h"
#include "device/DeviceManager.h"
#include "sync/SyncManager.h"
#include "ui/UiDispatcher.h"
#include "util/Json.h"
#include "util/Logger.h"

namespace cliplink {
namespace tray {

namespace {

constexpr const char* kTag = "TrayIcon";

constexpr wchar_t kWindowClass[] = L"ClipLinkTrayWindow";
constexpr wchar_t kWindowTitle[] = L"ClipLinkTray";

constexpr UINT_PTR kTrayId = 1;
// 托盘回调消息（NIM_ADD 的 uCallbackMessage）
constexpr UINT kMsgTray = WM_APP + 0x320;

// 菜单命令（TrackPopupMenu TPM_RETUCMD 直接返回命令 ID）
constexpr UINT kCmdOpenMain = 2001;
constexpr UINT kCmdPauseSync = 2002;
constexpr UINT kCmdSettings = 2003;
constexpr UINT kCmdToggleServer = 2004;
constexpr UINT kCmdExit = 2005;

}  // namespace

TrayIcon& TrayIcon::instance() {
    static TrayIcon tray;
    return tray;
}

std::wstring TrayIcon::buildTooltip() const {
    auto& sync = sync::SyncManager::instance();
    const wchar_t* state = L"断开";
    switch (sync.connectionState()) {
        case sync::ConnState::Connected:
            state = L"已连接";
            break;
        case sync::ConnState::Connecting:
            state = L"连接中";
            break;
        default:
            state = L"断开";
            break;
    }

    std::wstring tip = L"ClipLink\n服务器：";
    tip += state;
    if (sync.paused()) tip += L"（同步暂停）";
    tip += L"\n设备：";
    tip += util::utf8ToWide(DeviceManager::instance().deviceName());
    // NOTIFYICONDATA szTip 上限 128 字符（含结尾 \0）
    if (tip.size() > 126) tip.resize(126);
    return tip;
}

bool TrayIcon::create(HINSTANCE instance, Handlers handlers) {
    if (hwnd_ != nullptr) return true;
    instance_ = instance;
    handlers_ = std::move(handlers);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &TrayIcon::wndProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CLIPLOG_ERROR(kTag, "托盘窗口类注册失败, errno=" +
                                std::to_string(GetLastError()));
        return false;
    }

    // 独立隐藏消息窗口：承载托盘回调（不显示、无任务栏图标）
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kWindowTitle,
                            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                            this);
    if (hwnd_ == nullptr) {
        CLIPLOG_ERROR(kTag,
                      "托盘窗口创建失败, errno=" + std::to_string(GetLastError()));
        hwnd_ = nullptr;
        return false;
    }

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kMsgTray;
    // 应用图标（resource.rc 提供的 1 号图标，§97 至少正常图标）
    nid.hIcon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON),
                   GetSystemMetrics(SM_CYSMICON), 0));
    if (nid.hIcon == nullptr) {
        nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    const std::wstring tip = buildTooltip();
    wcsncpy(nid.szTip, tip.c_str(), std::size(nid.szTip) - 1);

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        CLIPLOG_ERROR(kTag, "托盘图标添加失败");
        destroy();
        return false;
    }
    added_ = true;

    ui::UiDispatcher::instance().registerWindow(hwnd_);
    CLIPLOG_INFO(kTag, "托盘图标已创建");
    return true;
}

void TrayIcon::destroy() {
    if (added_) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        added_ = false;
    }
    if (hwnd_ != nullptr) {
        ui::UiDispatcher::instance().unregisterWindow(hwnd_);
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
}

void TrayIcon::updateStatus() {
    if (!added_ || hwnd_ == nullptr) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP;
    const std::wstring tip = buildTooltip();
    wcsncpy(nid.szTip, tip.c_str(), std::size(nid.szTip) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::showContextMenu() {
    auto& sync = sync::SyncManager::instance();

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;

    // §66 菜单：ClipLink、●状态、打开主界面、暂停同步、设置、启动服务、退出
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"ClipLink");

    std::wstring stateLine = L"● ";
    switch (sync.connectionState()) {
        case sync::ConnState::Connected:
            stateLine += L"已连接";
            break;
        case sync::ConnState::Connecting:
            stateLine += L"连接中";
            break;
        default:
            stateLine += L"断开";
            break;
    }
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, stateLine.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, kCmdOpenMain, L"打开主界面");
    AppendMenuW(menu, MF_STRING | (sync.paused() ? MF_CHECKED : 0),
                kCmdPauseSync, L"暂停同步");
    AppendMenuW(menu, MF_STRING, kCmdSettings, L"设置");

    const AppConfig cfg = ConfigManager::instance().get();
    AppendMenuW(menu, MF_STRING | (cfg.serverEnabled ? MF_CHECKED : 0),
                kCmdToggleServer, L"启动服务");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"退出");

    SetForegroundWindow(hwnd_);
    POINT pt = {};
    GetCursorPos(&pt);
    // TPM_RETUCMD：命令作为返回值返回，免去 WM_COMMAND 中转
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
        hwnd_, nullptr));
    // 消除菜单关闭时的幽灵消息（MSDN 推荐做法）
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
        case kCmdOpenMain:
            if (handlers_.openMain) handlers_.openMain();
            break;
        case kCmdPauseSync:
            // §65：暂停后仍监听剪贴板、仍保存历史，仅不收发同步
            sync.setPaused(!sync.paused());
            updateStatus();
            ui::UiDispatcher::instance().broadcast(ui::kMsgConnState);
            CLIPLOG_INFO(kTag,
                         sync.paused() ? "托盘：同步已暂停" : "托盘：同步已恢复");
            break;
        case kCmdSettings:
            if (handlers_.openSettings) handlers_.openSettings();
            break;
        case kCmdToggleServer:
            applyToggleServer();
            break;
        case kCmdExit:
            if (handlers_.onExit) handlers_.onExit();
            break;
        default:
            break;
    }
}

void TrayIcon::applyToggleServer() {
    AppConfig cfg = ConfigManager::instance().get();
    cfg.serverEnabled = !cfg.serverEnabled;
    if (!ConfigManager::instance().update(cfg)) {
        CLIPLOG_ERROR(kTag, "服务开关配置写入失败");
        return;
    }

    auto& sync = sync::SyncManager::instance();
    if (sync.running()) {
        sync.stop();
        sync.start();
    }
    if (cfg.serverEnabled) {
        const std::string err = sync.serverLastError();
        if (!err.empty()) {
            // §67：端口冲突提示用户，不崩溃
            MessageBoxW(hwnd_,
                        util::utf8ToWide("启动服务失败：" + err).c_str(),
                        L"ClipLink", MB_OK | MB_ICONWARNING);
        }
    }
    CLIPLOG_INFO(kTag, std::string("托盘：启动服务=") +
                           (cfg.serverEnabled ? "开" : "关"));
    updateStatus();
    ui::UiDispatcher::instance().broadcast(ui::kMsgConfigChanged);
    ui::UiDispatcher::instance().broadcast(ui::kMsgConnState);
}

LRESULT CALLBACK TrayIcon::wndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam) {
    TrayIcon* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TrayIcon*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TrayIcon*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->handleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TrayIcon::handleMessage(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
    switch (msg) {
        case kMsgTray:
            switch (lParam) {
                case WM_LBUTTONDBLCLK:
                case WM_LBUTTONUP:
                    // §4：左键/双击打开主窗口
                    if (handlers_.openMain) handlers_.openMain();
                    return 0;
                case WM_RBUTTONUP:
                    showContextMenu();
                    return 0;
                default:
                    return 0;
            }
        case ui::kMsgConnState:
        case ui::kMsgConfigChanged:
            updateStatus();
            return 0;
        case WM_DESTROY:
            ui::UiDispatcher::instance().unregisterWindow(hwnd);
            hwnd_ = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace tray
}  // namespace cliplink

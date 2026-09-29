// ---------------------------------------------------------------------------
// 设置窗口实现（§36 设置页 / §5 开机启动 / §67 服务开关）
// ---------------------------------------------------------------------------

#include "ui/SettingsWindow.h"

#include <cstdlib>
#include <cwchar>

#include "config/ConfigManager.h"
#include "device/DeviceManager.h"
#include "sync/SyncManager.h"
#include "system/StartupManager.h"
#include "ui/UiDispatcher.h"
#include "util/Json.h"
#include "util/Logger.h"

namespace cliplink {
namespace ui {

namespace {

constexpr const char* kTag = "SettingsWindow";

constexpr wchar_t kWindowClass[] = L"ClipLinkSettingsWindow";
constexpr wchar_t kWindowTitle[] = L"ClipLink 设置";

// 控件 ID
constexpr int kIdAddr = 1101;
constexpr int kIdDeviceName = 1102;
constexpr int kIdServerEnabled = 1103;
constexpr int kIdPort = 1104;
constexpr int kIdConnState = 1105;
constexpr int kIdConnCount = 1106;
constexpr int kIdMaxCount = 1107;
constexpr int kIdMaxDays = 1108;
constexpr int kIdAutoStart = 1109;
constexpr int kIdSyncEnabled = 1110;
constexpr int kIdSave = 1111;
constexpr int kIdHint = 1112;
constexpr int kIdDayUnit = 1113;

constexpr int kDesignWidth = 340;
constexpr int kDesignHeight = 396;

int scaleForDpi(int value) {
    HDC dc = GetDC(nullptr);
    int dpi = 96;
    if (dc != nullptr) {
        dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
    }
    if (dpi <= 0) dpi = 96;
    return MulDiv(value, dpi, 96);
}

bool startsWithNoCase(const std::string& s, const char* prefix) {
    const size_t n = std::char_traits<char>::length(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

}  // namespace

SettingsWindow& SettingsWindow::instance() {
    static SettingsWindow win;
    return win;
}

HWND SettingsWindow::makeControl(const wchar_t* cls, const wchar_t* text,
                                 DWORD style, int x, int y, int w, int h,
                                 int id) {
    HWND hnd = CreateWindowExW(
        0, cls, text, WS_CHILD | WS_VISIBLE | style, scaleForDpi(x),
        scaleForDpi(y), scaleForDpi(w), scaleForDpi(h), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    if (hnd != nullptr) {
        SendMessageW(hnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    return hnd;
}

bool SettingsWindow::create(HINSTANCE instance) {
    if (hwnd_ != nullptr) return true;
    instance_ = instance;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &SettingsWindow::wndProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (wc.hIcon == nullptr) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CLIPLOG_ERROR(kTag, "窗口类注册失败, errno=" +
                                std::to_string(GetLastError()));
        return false;
    }

    const int w = scaleForDpi(kDesignWidth);
    const int h = scaleForDpi(kDesignHeight);

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kWindowTitle,
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                            CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr,
                            nullptr, instance, this);
    if (hwnd_ == nullptr) {
        CLIPLOG_ERROR(kTag,
                      "设置窗口创建失败, errno=" + std::to_string(GetLastError()));
        hwnd_ = nullptr;
        return false;
    }

    font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const int lblX = 10, ctlX = 150, ctlW = 178;
    int y = 12;

    // §36 字段布局
    makeControl(L"STATIC", L"服务器地址：", SS_LEFT, lblX, y + 4, 130, 18, 0);
    editAddr_ = makeControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                            ctlX, y, ctlW, 22, kIdAddr);
    y += 32;

    makeControl(L"STATIC", L"设备名称：", SS_LEFT, lblX, y + 4, 130, 18, 0);
    editDeviceName_ = makeControl(L"EDIT", L"",
                                  WS_BORDER | ES_AUTOHSCROLL, ctlX, y, ctlW,
                                  22, kIdDeviceName);
    y += 32;

    chkServerEnabled_ =
        makeControl(L"BUTTON", L"启动服务（本机 Server）",
                    BS_AUTOCHECKBOX, lblX, y, 250, 20, kIdServerEnabled);
    y += 30;

    makeControl(L"STATIC", L"服务端口：", SS_LEFT, lblX, y + 4, 130, 18, 0);
    editPort_ = makeControl(L"EDIT", L"",
                            WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, ctlX, y,
                            80, 22, kIdPort);
    y += 32;

    makeControl(L"STATIC", L"连接状态：", SS_LEFT, lblX, y + 4, 130, 18, 0);
    textConnState_ = makeControl(L"STATIC", L"断开", SS_LEFT, ctlX, y + 4,
                                 ctlW, 18, kIdConnState);
    y += 28;

    makeControl(L"STATIC", L"当前连接数：", SS_LEFT, lblX, y + 4, 130, 18, 0);
    textConnCount_ = makeControl(L"STATIC", L"0", SS_LEFT, ctlX, y + 4, ctlW,
                                 18, kIdConnCount);
    y += 30;

    makeControl(L"STATIC", L"保存记录最大条数：", SS_LEFT, lblX, y + 4, 130,
                18, 0);
    editMaxCount_ = makeControl(L"EDIT", L"",
                                WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, ctlX,
                                y, ctlW, 22, kIdMaxCount);
    y += 32;

    makeControl(L"STATIC", L"保存最长时间：", SS_LEFT, lblX, y + 4, 130, 18,
                0);
    editMaxDays_ = makeControl(L"EDIT", L"",
                               WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, ctlX,
                               y, 70, 22, kIdMaxDays);
    makeControl(L"STATIC", L"天", SS_LEFT, ctlX + 76, y + 4, 40, 18,
                kIdDayUnit);
    y += 32;

    chkAutoStart_ =
        makeControl(L"BUTTON", L"开机自动启动", BS_AUTOCHECKBOX, lblX, y,
                    150, 20, kIdAutoStart);
    chkSyncEnabled_ =
        makeControl(L"BUTTON", L"自动同步剪贴板", BS_AUTOCHECKBOX, lblX + 150,
                    y, 170, 20, kIdSyncEnabled);
    y += 34;

    btnSave_ = makeControl(L"BUTTON", L"保存", BS_DEFPUSHBUTTON, lblX, y, 90,
                           26, kIdSave);
    textHint_ = makeControl(L"STATIC", L"", SS_LEFT, lblX + 100, y + 6, 220,
                            18, kIdHint);

    if (editAddr_ == nullptr || editDeviceName_ == nullptr ||
        chkServerEnabled_ == nullptr || editPort_ == nullptr ||
        editMaxCount_ == nullptr || editMaxDays_ == nullptr ||
        chkAutoStart_ == nullptr || chkSyncEnabled_ == nullptr ||
        btnSave_ == nullptr) {
        CLIPLOG_ERROR(kTag, "子控件创建失败, errno=" +
                                std::to_string(GetLastError()));
        destroy();
        return false;
    }

    UiDispatcher::instance().registerWindow(hwnd_);
    loadFromConfig();
    updateLiveStatus();
    CLIPLOG_INFO(kTag, "设置窗口创建完成");
    return true;
}

void SettingsWindow::layoutControls() {
    // 控件在 makeControl 内按 DPI 缩放定位，窗口固定尺寸无需重排；
    // 保留 WM_SIZE 处理以防系统缩放变化
}

void SettingsWindow::loadFromConfig() {
    const AppConfig cfg = ConfigManager::instance().get();

    SetWindowTextW(editAddr_,
                   util::utf8ToWide(cfg.serverAddress).c_str());
    SetWindowTextW(editDeviceName_,
                   util::utf8ToWide(DeviceManager::instance().deviceName())
                       .c_str());
    SendMessageW(chkServerEnabled_, BM_SETCHECK,
                 cfg.serverEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(editPort_, std::to_wstring(cfg.port).c_str());
    SetWindowTextW(editMaxCount_, std::to_wstring(cfg.maxHistoryCount).c_str());
    SetWindowTextW(editMaxDays_, std::to_wstring(cfg.maxHistoryDays).c_str());
    SendMessageW(chkAutoStart_, BM_SETCHECK,
                 cfg.autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(chkSyncEnabled_, BM_SETCHECK,
                 cfg.syncEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    setHint(L"");
}

std::wstring SettingsWindow::getText(HWND edit) {
    if (edit == nullptr) return std::wstring();
    const int len = GetWindowTextLengthW(edit);
    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    if (len > 0) GetWindowTextW(edit, buf.data(), len + 1);
    buf.resize(wcslen(buf.c_str()));
    return buf;
}

bool SettingsWindow::parseInt(const std::wstring& s, int* out) {
    if (s.empty() || out == nullptr) return false;
    wchar_t* end = nullptr;
    const long v = wcstol(s.c_str(), &end, 10);
    if (end == nullptr || *end != L'\0') return false;
    if (v < 0 || v > 100000000L) return false;
    *out = static_cast<int>(v);
    return true;
}

void SettingsWindow::saveToConfig() {
    // ---- 读取与校验（§36 全字段）--------------------------------------
    const std::wstring addrW = getText(editAddr_);
    const std::wstring nameW = getText(editDeviceName_);
    const std::wstring portW = getText(editPort_);
    const std::wstring maxCountW = getText(editMaxCount_);
    const std::wstring maxDaysW = getText(editMaxDays_);

    const std::string addr = util::wideToUtf8(addrW);
    if (addr.empty() || (!startsWithNoCase(addr, "ws://") &&
                         !startsWithNoCase(addr, "wss://"))) {
        setHint(L"服务器地址需以 ws:// 或 wss:// 开头");
        MessageBoxW(hwnd_, L"服务器地址格式不正确。\r\n"
                           L"示例：ws://192.168.1.100:9000（§37，MVP 用 ws://）",
                    kWindowTitle, MB_OK | MB_ICONWARNING);
        return;
    }
    if (nameW.empty()) {
        setHint(L"设备名称不能为空");
        MessageBoxW(hwnd_, L"设备名称不能为空（§20）。", kWindowTitle,
                    MB_OK | MB_ICONWARNING);
        return;
    }
    int port = 0, maxCount = 0, maxDays = 0;
    if (!parseInt(portW, &port) || port < 1 || port > 65535) {
        setHint(L"端口需为 1-65535");
        MessageBoxW(hwnd_, L"服务端口需为 1-65535 的整数。", kWindowTitle,
                    MB_OK | MB_ICONWARNING);
        return;
    }
    if (!parseInt(maxCountW, &maxCount) || maxCount < 1) {
        setHint(L"最大条数需为正整数");
        MessageBoxW(hwnd_, L"保存记录最大条数需为正整数（§28）。",
                    kWindowTitle, MB_OK | MB_ICONWARNING);
        return;
    }
    if (!parseInt(maxDaysW, &maxDays) || maxDays < 1) {
        setHint(L"保存天数需为正整数");
        MessageBoxW(hwnd_, L"保存最长时间需为正整数（天，§28）。",
                    kWindowTitle, MB_OK | MB_ICONWARNING);
        return;
    }

    const bool serverEnabled =
        SendMessageW(chkServerEnabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool autoStart =
        SendMessageW(chkAutoStart_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool syncEnabled =
        SendMessageW(chkSyncEnabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;

    const AppConfig oldCfg = ConfigManager::instance().get();
    const bool netChanged = (oldCfg.serverAddress != addr) ||
                            (oldCfg.serverEnabled != serverEnabled) ||
                            (oldCfg.port != port);

    // ---- 应用生效（§62：只经业务层接口）-------------------------------
    AppConfig cfg = oldCfg;
    cfg.serverAddress = addr;
    cfg.serverEnabled = serverEnabled;
    cfg.port = port;
    cfg.maxHistoryCount = maxCount;
    cfg.maxHistoryDays = maxDays;
    cfg.autoStart = autoStart;
    cfg.syncEnabled = syncEnabled;

    if (!ConfigManager::instance().update(cfg)) {
        setHint(L"配置写入失败");
        MessageBoxW(hwnd_, L"配置写入失败，详情见日志。", kWindowTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }

    // 设备名：经 DeviceManager 持久化（§20）
    const std::string nameUtf8 = util::wideToUtf8(nameW);
    if (nameUtf8 != DeviceManager::instance().deviceName() &&
        !DeviceManager::instance().setDeviceName(nameUtf8)) {
        CLIPLOG_WARN(kTag, "设备名称保存失败");
    }

    // 开机自启动：HKCU Run 键（§5，无需管理员；失败提示不崩溃）
    if (!system::StartupManager::instance().setEnabled(autoStart)) {
        setHint(L"配置已保存；开机启动项写入失败");
        MessageBoxW(hwnd_,
                    L"设置已保存，但开机自动启动项写入失败。\r\n"
                    L"请检查权限或查看日志。",
                    kWindowTitle, MB_OK | MB_ICONWARNING);
    }

    auto& sync = sync::SyncManager::instance();
    if (netChanged && sync.running()) {
        // 服务端/地址/端口变化：重启同步层应用生效（§67 端口冲突仅提示）
        sync.stop();
        sync.start();
        const std::string err = sync.serverLastError();
        if (serverEnabled && !err.empty()) {
            MessageBoxW(hwnd_,
                        util::utf8ToWide("启动服务失败：" + err).c_str(),
                        kWindowTitle, MB_OK | MB_ICONWARNING);
        }
    } else {
        // 暂停开关独立生效（§65）；未变化时 setPaused 也幂等无副作用
        sync.setPaused(!syncEnabled);
    }

    // 通知托盘与主窗口刷新（Tooltip / 菜单勾选 / 连接状态）
    UiDispatcher::instance().broadcast(kMsgConfigChanged);
    UiDispatcher::instance().broadcast(kMsgConnState);

    setHint(L"设置已保存");
    CLIPLOG_INFO(kTag, "设置已保存: 服务器=" + addr +
                           ", 端口=" + std::to_string(port) +
                           ", 启动服务=" +
                           std::string(serverEnabled ? "是" : "否") +
                           ", 开机启动=" +
                           std::string(autoStart ? "是" : "否"));
}

void SettingsWindow::setHint(const wchar_t* text) {
    if (textHint_ != nullptr) SetWindowTextW(textHint_, text);
}

void SettingsWindow::updateLiveStatus() {
    if (hwnd_ == nullptr) return;
    auto& sync = sync::SyncManager::instance();

    const wchar_t* stateText = L"断开";
    switch (sync.connectionState()) {
        case sync::ConnState::Connected:
            stateText = L"已连接";
            break;
        case sync::ConnState::Connecting:
            stateText = L"连接中";
            break;
        case sync::ConnState::Disconnected:
        default:
            stateText = L"断开";
            break;
    }
    std::wstring state(stateText);
    if (sync.paused()) state += L"（同步暂停）";
    if (!sync.running()) state += L"（同步层未启动）";
    SetWindowTextW(textConnState_, state.c_str());

    const size_t count = sync.serverConnectionCount();
    const size_t online = sync.serverOnlineDeviceCount();
    SetWindowTextW(textConnCount_,
                   (std::to_wstring(count) + L"（在线设备 " +
                    std::to_wstring(online) + L"）")
                       .c_str());
}

void SettingsWindow::show() {
    if (hwnd_ == nullptr) return;
    loadFromConfig();
    updateLiveStatus();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    // 窗口可见期间低频刷新连接数（WebSocketServer 无变更回调；§3 禁高频）
    SetTimer(hwnd_, kTimerId, kTimerIntervalMs, nullptr);
}

void SettingsWindow::hide() {
    if (hwnd_ == nullptr) return;
    KillTimer(hwnd_, kTimerId);
    ShowWindow(hwnd_, SW_HIDE);
}

bool SettingsWindow::visible() const {
    return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE;
}

void SettingsWindow::destroy() {
    if (hwnd_ == nullptr) return;
    KillTimer(hwnd_, kTimerId);
    UiDispatcher::instance().unregisterWindow(hwnd_);
    HWND h = hwnd_;
    hwnd_ = nullptr;
    DestroyWindow(h);
}

LRESULT CALLBACK SettingsWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->handleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT SettingsWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wParam) == kIdSave && HIWORD(wParam) == BN_CLICKED) {
                saveToConfig();
                return 0;
            }
            return 0;
        case kMsgConnState:
            updateLiveStatus();
            return 0;
        case kMsgConfigChanged:
            updateLiveStatus();
            return 0;
        case WM_TIMER:
            if (wParam == kTimerId) {
                // 仅可见期间低频刷新连接数（2s），不做任何数据库轮询
                updateLiveStatus();
                return 0;
            }
            break;
        case WM_CLOSE:
            hide();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            UiDispatcher::instance().unregisterWindow(hwnd);
            hwnd_ = nullptr;
            editAddr_ = editDeviceName_ = chkServerEnabled_ = editPort_ =
                textConnState_ = textConnCount_ = editMaxCount_ =
                    editMaxDays_ = chkAutoStart_ = chkSyncEnabled_ =
                        btnSave_ = textHint_ = nullptr;
            font_ = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace ui
}  // namespace cliplink

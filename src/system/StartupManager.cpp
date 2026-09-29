// ---------------------------------------------------------------------------
// 开机自启动管理实现（§5：HKCU Run 键，仅当前用户、免管理员）
// ---------------------------------------------------------------------------

#include "system/StartupManager.h"

#include <windows.h>

#include <string>
#include <vector>

#include "util/Json.h"
#include "util/Logger.h"

namespace cliplink {
namespace system {

namespace {

constexpr const char* kTag = "StartupManager";

constexpr wchar_t kRunKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// 当前进程可执行文件绝对路径（UTF-8），失败返回空串
std::string currentExePath() {
    std::vector<wchar_t> buf(MAX_PATH + 2);
    DWORD len = GetModuleFileNameW(nullptr, buf.data(),
                                   static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) return std::string();
    return util::wideToUtf8(std::wstring(buf.data(), len));
}

}  // namespace

StartupManager& StartupManager::instance() {
    static StartupManager mgr;
    return mgr;
}

const char* StartupManager::valueName() {
    return "ClipLink";
}

bool StartupManager::isEnabled() const {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0,
                            KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS) return false;

    BOOL present = FALSE;
    rc = RegQueryValueExW(key, L"ClipLink", nullptr, nullptr, nullptr,
                          nullptr);
    present = (rc == ERROR_SUCCESS);
    RegCloseKey(key);
    return present != FALSE;
}

bool StartupManager::setEnabled(bool enable) {
    HKEY key = nullptr;
    // KEY_SET_VALUE 仅需当前用户权限，不需要管理员（§5）
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0,
                              KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key,
                              nullptr);
    if (rc != ERROR_SUCCESS) {
        CLIPLOG_ERROR(kTag,
                      "打开 HKCU Run 键失败, rc=" + std::to_string(rc));
        return false;
    }

    bool ok = true;
    if (enable) {
        const std::string exe = currentExePath();
        if (exe.empty()) {
            CLIPLOG_ERROR(kTag, "获取可执行文件路径失败，无法写入自启动");
            RegCloseKey(key);
            return false;
        }
        // 带引号包裹路径，避免路径含空格被拆分
        const std::wstring value = L"\"" + util::utf8ToWide(exe) + L"\"";
        rc = RegSetValueExW(key, L"ClipLink", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>(
                                (value.size() + 1) * sizeof(wchar_t)));
        if (rc != ERROR_SUCCESS) {
            CLIPLOG_ERROR(kTag,
                          "写入自启动项失败, rc=" + std::to_string(rc));
            ok = false;
        } else {
            CLIPLOG_INFO(kTag, "已开启开机自动启动: " + exe);
        }
    } else {
        rc = RegDeleteValueW(key, L"ClipLink");
        // 文件本就不存在也算成功（幂等关闭）
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
            CLIPLOG_ERROR(kTag,
                          "删除自启动项失败, rc=" + std::to_string(rc));
            ok = false;
        } else {
            CLIPLOG_INFO(kTag, "已关闭开机自动启动");
        }
    }

    RegCloseKey(key);
    return ok;
}

}  // namespace system
}  // namespace cliplink

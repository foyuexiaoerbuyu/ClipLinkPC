#include "ConfigManager.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <sstream>

#include "../common/DataTypes.h"
#include "../util/Json.h"
#include "../util/Logger.h"

namespace cliplink {

namespace {

// %APPDATA% 目录（UTF-8），失败返回空串
std::wstring queryAppData() {
    wchar_t buf[MAX_PATH + 1] = {0};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::wstring(buf, n);
    }
    // 兜底：SHGetFolderPathW
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, buf))) {
        return std::wstring(buf);
    }
    return std::wstring();
}

bool createDirTree(const std::wstring& dir) {
    if (CreateDirectoryW(dir.c_str(), nullptr) ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
        return true;
    }
    std::wstring path;
    for (size_t i = 0; i < dir.size(); ++i) {
        wchar_t c = dir[i];
        if ((c == L'\\' || c == L'/') && !path.empty() && path.back() != L':') {
            CreateDirectoryW(path.c_str(), nullptr);
        }
        path.push_back(c);
    }
    return CreateDirectoryW(path.c_str(), nullptr) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

bool readFileUtf8(const std::wstring& wpath, std::string& out) {
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
        size.QuadPart > 10ll * 1024ll * 1024ll) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    BOOL ok = TRUE;
    if (!out.empty()) {
        ok = ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
    }
    CloseHandle(h);
    if (!ok || read != out.size()) {
        return false;
    }
    // 去除 UTF-8 BOM
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

}  // namespace

ConfigManager& ConfigManager::instance() {
    static ConfigManager inst;
    return inst;
}

void ConfigManager::ensureDirLocked() {
    if (dirReady_) {
        return;
    }
    std::wstring base = queryAppData();
    if (base.empty()) {
        base = L".";
    }
    std::wstring wdir = base + L"\\ClipLink";
    createDirTree(wdir);
    dir_  = util::wideToUtf8(wdir);
    path_ = dir_ + "\\config.json";
    dirReady_ = true;
}

const std::string& ConfigManager::configDir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const_cast<ConfigManager*>(this)->ensureDirLocked();
    return dir_;
}

const std::string& ConfigManager::configPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const_cast<ConfigManager*>(this)->ensureDirLocked();
    return path_;
}

bool ConfigManager::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureDirLocked();

    std::wstring wpath = util::utf8ToWide(path_);
    std::string text;
    if (!readFileUtf8(wpath, text)) {
        // 首次运行：以默认值创建配置文件
        return saveLocked();
    }

    util::JsonValue root;
    std::string err;
    if (!util::JsonValue::parse(text, root, &err) || !root.isObject()) {
        CLIPLOG_WARN("config", "config.json 解析失败，使用默认配置: " + err);
        return false;
    }

    config_.deviceId   = root.get("deviceId").asString();
    config_.deviceName = root.get("deviceName").asString();
    config_.serverAddress =
        root.get("serverAddress").asString("ws://127.0.0.1:" +
                                           std::to_string(constants::kDefaultPort));
    config_.serverEnabled = root.get("serverEnabled").asBool(false);
    config_.autoStart     = root.get("autoStart").asBool(false);
    config_.maxHistoryCount =
        root.get("maxHistoryCount").asInt(constants::kDefaultMaxHistoryCount);
    config_.maxHistoryDays =
        root.get("maxHistoryDays").asInt(constants::kDefaultMaxHistoryDays);
    config_.port = root.get("port").asInt(constants::kDefaultPort);
    config_.syncEnabled = root.get("syncEnabled").asBool(true);

    // 数值边界保护
    if (config_.maxHistoryCount <= 0) {
        config_.maxHistoryCount = constants::kDefaultMaxHistoryCount;
    }
    if (config_.maxHistoryDays <= 0) {
        config_.maxHistoryDays = constants::kDefaultMaxHistoryDays;
    }
    if (config_.port < 1 || config_.port > 65535) {
        config_.port = constants::kDefaultPort;
    }
    if (config_.serverAddress.empty()) {
        config_.serverAddress = "ws://127.0.0.1:" + std::to_string(config_.port);
    }
    return true;
}

bool ConfigManager::save() {
    std::lock_guard<std::mutex> lock(mutex_);
    return saveLocked();
}

bool ConfigManager::saveLocked() {
    ensureDirLocked();

    util::JsonValue root = util::JsonValue::makeObject();
    root.set("deviceId", config_.deviceId);
    root.set("deviceName", config_.deviceName);
    root.set("serverAddress", config_.serverAddress);
    root.set("serverEnabled", config_.serverEnabled);
    root.set("autoStart", config_.autoStart);
    root.set("maxHistoryCount", static_cast<int64_t>(config_.maxHistoryCount));
    root.set("maxHistoryDays", static_cast<int64_t>(config_.maxHistoryDays));
    root.set("port", static_cast<int64_t>(config_.port));
    root.set("syncEnabled", config_.syncEnabled);

    std::string text = root.dump(2);
    text.push_back('\n');

    // 先写 tmp，再原子替换，防止写一半损坏原配置
    std::wstring wtmp  = util::utf8ToWide(path_ + ".tmp");
    std::wstring wpath = util::utf8ToWide(path_);

    HANDLE h = CreateFileW(wtmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        CLIPLOG_ERROR("config", "配置文件创建失败: " + path_ + ".tmp");
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()),
                        &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != text.size()) {
        CLIPLOG_ERROR("config", "配置文件写入失败: " + path_ + ".tmp");
        return false;
    }
    if (!MoveFileExW(wtmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        CLIPLOG_ERROR("config", "配置文件替换失败: " + path_);
        return false;
    }
    return true;
}

AppConfig ConfigManager::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

bool ConfigManager::update(const AppConfig& config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
    }
    return save();
}

}  // namespace cliplink

#include "Logger.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <share.h>  // _wfsopen / _SH_DENYNO

#include "Json.h"

namespace cliplink {
namespace util {

namespace {

// %APPDATA% 获取（失败时回退到当前用户 Profile）
std::wstring getAppDataDir() {
    wchar_t buf[MAX_PATH + 1] = {0};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::wstring(buf, n);
    }
    n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::wstring(buf, n) + L"\\AppData\\Roaming";
    }
    return L".";
}

const char* levelName(LogLevel lv) {
    switch (lv) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

std::string localTimestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

void ensureDirExists(const std::wstring& dir) {
    if (CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return;
    }
    // 逐级创建
    std::wstring path;
    for (size_t i = 0; i < dir.size(); ++i) {
        wchar_t c = dir[i];
        if (c == L'\\' || c == L'/') {
            if (!path.empty() && path.back() != L':') {
                CreateDirectoryW(path.c_str(), nullptr);
            }
        }
        path.push_back(c);
    }
    CreateDirectoryW(path.c_str(), nullptr);
}

}  // namespace

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    shutdown();
}

void Logger::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    initLocked();
}

void Logger::initLocked() {
    if (initialized_) {
        return;
    }
    std::wstring base = getAppDataDir();
    std::wstring wdir = base + L"\\ClipLink\\logs";
    ensureDirExists(wdir);
    dir_  = wideToUtf8(wdir);
    path_ = dir_ + "\\cliplink.log";
    initialized_ = openFile();
}

bool Logger::openFile() {
    std::wstring wpath = utf8ToWide(path_);
    FILE* f = nullptr;
    // "ab" 二进制追加：日志行已是 UTF-8 字节，避免 ccs 转换层重复编码。
    // 优先 _wfsopen(_SH_DENYNO)：显式授予共享读写，允许外部工具
    // （记事本 / Get-Content / tail 脚本）在程序运行期间实时读取日志（§71）。
    f = _wfsopen(wpath.c_str(), L"ab", _SH_DENYNO);
    if (f == nullptr) {
        // 兜底1：常规 _wfopen_s（部分工具链下其句柄拒绝共享读）
        _wfopen_s(&f, wpath.c_str(), L"ab");
    }
    if (f == nullptr) {
        // 兜底2：ANSI 路径打开
        f = std::fopen(path_.c_str(), "ab");
    }
    if (f == nullptr) {
        return false;
    }
    file_ = f;
    return true;
}

void Logger::rotateIfNeeded() {
    if (file_ == nullptr) {
        return;
    }
    FILE* f = static_cast<FILE*>(file_);
    long pos = std::ftell(f);
    if (pos < 0 || static_cast<uint64_t>(pos) < maxBytes_) {
        return;
    }
    std::fclose(f);
    file_ = nullptr;

    // cliplink.log -> cliplink.log.1（覆盖旧滚动文件）
    std::wstring wpath = utf8ToWide(path_);
    std::wstring wold  = utf8ToWide(path_ + ".1");
    _wremove(wold.c_str());
    _wrename(wpath.c_str(), wold.c_str());

    openFile();
}

void Logger::setLevel(LogLevel lv) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = lv;
}

bool Logger::initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

const std::string& Logger::filePath() const {
    return path_;
}

void Logger::log(LogLevel lv, const std::string& tag, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        // init() 尚未调用（入口早期日志）时就地初始化，保证日志不丢
        initLocked();
        if (!initialized_) {
            return;
        }
    }
    if (static_cast<int>(lv) < static_cast<int>(level_)) {
        return;
    }

    rotateIfNeeded();
    if (file_ == nullptr) {
        return;
    }

    std::string line = localTimestamp();
    line += " [";
    line += levelName(lv);
    line += "] [";
    line += tag;
    line += "] ";
    line += message;
    line += "\n";

    FILE* f = static_cast<FILE*>(file_);
    std::fwrite(line.data(), 1, line.size(), f);
    // 每条日志立即落盘：MVP 日志量极小，避免崩溃/断电丢日志
    std::fflush(f);
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != nullptr) {
        std::fflush(static_cast<FILE*>(file_));
        std::fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
    initialized_ = false;
}

}  // namespace util
}  // namespace cliplink

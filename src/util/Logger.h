#pragma once

// ---------------------------------------------------------------------------
// 轻量级日志（INFO / WARN / ERROR，可选 DEBUG）
// - 单文件写入，超过 5MB 滚动为 cliplink.log.1（见需求 §71）
// - 线程安全（Clipboard / WebSocket / UI 线程共用）
// - 不记录剪贴板内容的责任由调用方约束：禁止传入完整敏感内容
// ---------------------------------------------------------------------------

#include <cstdint>
#include <mutex>
#include <string>

#include "../common/DataTypes.h"

namespace cliplink {
namespace util {

enum class LogLevel {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3
};

class Logger {
public:
    static Logger& instance();

    // 初始化：创建 %APPDATA%\ClipLink\logs\ 并打开 cliplink.log
    // 重复调用安全；初始化失败时降级为无文件输出（程序不退出）
    void init();

    void setLevel(LogLevel lv);
    bool initialized() const;

    void log(LogLevel lv, const std::string& tag, const std::string& message);

    void shutdown();

    // 当前日志文件完整路径（UTF-8）
    const std::string& filePath() const;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger() = default;
    ~Logger();

    void rotateIfNeeded();
    bool openFile();
    void initLocked();  // 调用方必须已持有 mutex_

    mutable std::mutex mutex_;
    void*    file_ = nullptr;  // FILE*，避免头文件引入 stdio
    bool     initialized_ = false;
    LogLevel level_ = LogLevel::Info;
    uint64_t maxBytes_ = constants::kMaxLogBytes;
    std::string dir_;
    std::string path_;
};

}  // namespace util
}  // namespace cliplink

#define CLIPLOG_DEBUG(tag, msg) \
    ::cliplink::util::Logger::instance().log(::cliplink::util::LogLevel::Debug, tag, msg)
#define CLIPLOG_INFO(tag, msg) \
    ::cliplink::util::Logger::instance().log(::cliplink::util::LogLevel::Info, tag, msg)
#define CLIPLOG_WARN(tag, msg) \
    ::cliplink::util::Logger::instance().log(::cliplink::util::LogLevel::Warn, tag, msg)
#define CLIPLOG_ERROR(tag, msg) \
    ::cliplink::util::Logger::instance().log(::cliplink::util::LogLevel::Error, tag, msg)

#pragma once

// ---------------------------------------------------------------------------
// 本地配置管理
// - 配置文件：%APPDATA%\ClipLink\config.json（见需求 §24 / §27）
// - 字段：deviceId / deviceName / serverAddress / serverEnabled / autoStart /
//         maxHistoryCount / maxHistoryDays / port / syncEnabled
// - 线程安全；save() 采用 tmp + 原子替换，避免断电产生半截文件
// ---------------------------------------------------------------------------

#include <mutex>
#include <string>

namespace cliplink {

struct AppConfig {
    std::string deviceId;       // 首次启动生成的 UUID，持久化（§19）
    std::string deviceName;     // 默认 Windows 计算机名（§20）
    std::string serverAddress = "ws://127.0.0.1:9000";  // 与 constants::kDefaultPort 一致
    bool serverEnabled = false; // 是否启动内置 WebSocket Server（§34）
    bool autoStart = false;     // 开机自动启动（§5）
    int  maxHistoryCount = 1000;  // 历史最大条数（§28）
    int  maxHistoryDays  = 30;    // 历史最长保存天数（§28）
    int  port = 9000;             // Server 监听端口（§34）
    bool syncEnabled = true;      // 自动同步剪贴板（§36）
};

class ConfigManager {
public:
    static ConfigManager& instance();

    // 读取 config.json；文件不存在时以默认值创建，解析失败时保留默认并记录日志
    bool load();

    // 序列化并原子写回（tmp 文件 + MoveFileEx 替换）
    bool save();

    // 线程安全地获取 / 更新配置（update 内部立即落盘）
    AppConfig get() const;
    bool update(const AppConfig& config);

    // %APPDATA%\ClipLink（UTF-8）
    const std::string& configDir() const;
    // %APPDATA%\ClipLink\config.json（UTF-8）
    const std::string& configPath() const;

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

private:
    ConfigManager() = default;

    void ensureDirLocked();
    bool saveLocked();  // 调用方必须已持有 mutex_

    mutable std::mutex mutex_;
    AppConfig config_;
    std::string dir_;
    std::string path_;
    bool dirReady_ = false;
};

}  // namespace cliplink

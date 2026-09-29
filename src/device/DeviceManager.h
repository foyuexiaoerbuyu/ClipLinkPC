#pragma once

// ---------------------------------------------------------------------------
// 设备标识管理
// - deviceId：首次启动生成 UUID 并持久化到 config.json，重启不变（§19 / §110）
// - deviceName：用户可配置，默认取 Windows 计算机名（§20）
// ---------------------------------------------------------------------------

#include <mutex>
#include <string>

namespace cliplink {

class DeviceManager {
public:
    static DeviceManager& instance();

    // 确保 deviceId / deviceName 有效：缺失则生成（计算机名）并保存
    bool initialize();

    std::string deviceId() const;
    std::string deviceName() const;

    // 修改设备名并持久化；空串视为非法，返回 false
    bool setDeviceName(const std::string& name);

    bool initialized() const;

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

private:
    DeviceManager() = default;

    // Windows 计算机名（UTF-8），失败返回 "Windows-PC"
    static std::string queryComputerName();

    mutable std::mutex mutex_;
    std::string deviceId_;
    std::string deviceName_;
    bool initialized_ = false;
};

}  // namespace cliplink

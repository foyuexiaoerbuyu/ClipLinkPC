#include "DeviceManager.h"

#include <windows.h>

#include "../config/ConfigManager.h"
#include "../util/Json.h"
#include "../util/Logger.h"
#include "../util/uuid.h"

namespace cliplink {

DeviceManager& DeviceManager::instance() {
    static DeviceManager inst;
    return inst;
}

std::string DeviceManager::queryComputerName() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buf, &size) && size > 0) {
        std::string name = util::wideToUtf8(std::wstring(buf, size));
        if (!name.empty()) {
            return name;
        }
    }
    return "Windows-PC";
}

bool DeviceManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return true;
    }

    AppConfig cfg = ConfigManager::instance().get();
    bool changed = false;

    if (cfg.deviceId.empty()) {
        cfg.deviceId = util::uuidV4();
        changed = true;
        CLIPLOG_INFO("device", "首次启动生成 deviceId: " + cfg.deviceId);
    }
    if (cfg.deviceName.empty()) {
        cfg.deviceName = queryComputerName();
        changed = true;
        CLIPLOG_INFO("device", "默认设备名取计算机名: " + cfg.deviceName);
    }

    if (changed && !ConfigManager::instance().update(cfg)) {
        CLIPLOG_ERROR("device", "设备信息持久化失败");
        return false;
    }

    deviceId_   = cfg.deviceId;
    deviceName_ = cfg.deviceName;
    initialized_ = true;
    return true;
}

std::string DeviceManager::deviceId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deviceId_;
}

std::string DeviceManager::deviceName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deviceName_;
}

bool DeviceManager::setDeviceName(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            return false;
        }
        deviceName_ = name;
    }
    AppConfig cfg = ConfigManager::instance().get();
    cfg.deviceName = name;
    if (!ConfigManager::instance().update(cfg)) {
        CLIPLOG_ERROR("device", "设备名保存失败");
        return false;
    }
    CLIPLOG_INFO("device", "设备名更新为: " + name);
    return true;
}

bool DeviceManager::initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

}  // namespace cliplink

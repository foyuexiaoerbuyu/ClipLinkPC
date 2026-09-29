#pragma once

// ---------------------------------------------------------------------------
// 开机自启动管理（见需求 §5）
// - 机制：HKCU\Software\Microsoft\Windows\CurrentVersion\Run
// - 仅当前用户、无需管理员权限、可在设置中关闭
// - 卸载/删除程序时可清理：setEnabled(false) 即删除对应注册表项
// ---------------------------------------------------------------------------

namespace cliplink {
namespace system {

class StartupManager {
public:
    static StartupManager& instance();

    // 查询当前用户 Run 键下是否已配置 ClipLink 自启动
    bool isEnabled() const;

    // 写入/删除自启动项；enable=false 时删除（即卸载清理路径）
    // 注册表写入失败返回 false（不抛异常、不崩溃，§72）
    bool setEnabled(bool enable);

    // 注册表值名（与 resource / 文档保持一致）
    static const char* valueName();

    StartupManager(const StartupManager&) = delete;
    StartupManager& operator=(const StartupManager&) = delete;

private:
    StartupManager() = default;
};

}  // namespace system
}  // namespace cliplink

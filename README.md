---
AIGC:
    Label: "1"
    ContentProducer: 001191440300708461136T1XGW3
    ProduceID: 58e87cd5ac29bab7322874bd6b207f1c_d5221a57bbb911f18442525400de85a5
    ReservedCode1: PGirGCx3mmDI2xKTqvzkiEKfDqvVsMv8k1JXVpRmNPplBu/FBJl8CEibCJl38UxlkCtx7LZFIipqgvtUb0wSprYoEqFKWWNQqk8mniZbQ6fw2LIu8Yd6oP0HTGXXcEcyoVGF1/IOApstVoYpnQupmyXL/11dfPYHWdA3FQ4CnkuG6kvl3BpLSJTtx+0=
    ContentPropagator: 001191440300708461136T1XGW3
    PropagateID: 58e87cd5ac29bab7322874bd6b207f1c_d5221a57bbb911f18442525400de85a5
    ReservedCode2: PGirGCx3mmDI2xKTqvzkiEKfDqvVsMv8k1JXVpRmNPplBu/FBJl8CEibCJl38UxlkCtx7LZFIipqgvtUb0wSprYoEqFKWWNQqk8mniZbQ6fw2LIu8Yd6oP0HTGXXcEcyoVGF1/IOApstVoYpnQupmyXL/11dfPYHWdA3FQ4CnkuG6kvl3BpLSJTtx+0=
---

# ClipLink PC（Windows MVP）

ClipLink 是一款 Windows 剪贴板同步工具：后台低功耗监听系统剪贴板，通过
WebSocket 与服务器同步，并提供本地剪贴板历史记录。本仓库为 **PC 端
（ClipLinkPC）** 项目，技术路线为 **C++17 + Win32 + WebSocket + SQLite**，
最终产物为单个 `ClipLink.exe`，不依赖 Qt / Electron 等任何 UI 框架。

> 需求文档：`D:\02Code\06Ai_project\ClipLink Windows MVP 完整需求文档.txt`

## 功能特性

- **事件驱动剪贴板监听**：`AddClipboardFormatListener` + `WM_CLIPBOARDUPDATE`，
  无任何轮询，空闲时 CPU 接近 0%（§2 / §103）
- **本地剪贴板历史**：SQLite 保存，主窗口按 `created_at DESC` 展示，
  点击/双击历史项写回系统剪贴板（HISTORY 来源：不发 WS、不新增历史，§76/§83）
- **WebSocket 双角色**：同一程序可同时作为 Server（`0.0.0.0:端口`）与
  Client（连接任意 `ws://` / `wss://` 地址），支持多客户端广播
- **同步安全**：SHA-256 内容哈希 + suppress 标志双层防循环；UUID `eventId`
  幂等去重；`serverSeq` 全局顺序 + `lastServerSeq` 离线增量补同步
- **断网缓存**：离线期间事件写入本地 `sync_status=PENDING`，重连后自动上传；
  指数退避自动重连（1/2/5/10/30s，§39）
- **常驻托盘**：启动默认隐藏到托盘，关闭窗口仅隐藏（§64）；托盘菜单含
  打开主界面 / 暂停同步 / 设置 / 启动服务 / 退出
- **开机自动启动**：`HKCU\...\Run` 写入，仅当前用户、无需管理员、可关闭（§5）
- **200 × 500 主窗口**：纯 Win32 控件，DPI 缩放适配（§6）
- **历史清理**：按最大条数与最长保存天数自动清理（§28）
- **轻量日志**：`%APPDATA%\ClipLink\logs\cliplink.log`，5MB 滚动，
  程序运行期间可被外部工具实时读取

## 目录结构

```
ClipLinkPC/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main/        应用入口（装配日志/配置/设备/数据库/剪贴板/同步/托盘/UI/开机启动）
│   ├── common/      公共数据类型与常量（DataTypes.h）
│   ├── util/        SHA256 / UUID / Logger / JSON / 字符串转换
│   ├── config/      ConfigManager（config.json 原子读写）
│   ├── device/      DeviceManager（deviceId / deviceName 持久化）
│   ├── database/    DatabaseManager / ClipboardRepository（SQLite）
│   ├── clipboard/   ClipboardMonitor / ClipboardReader / ClipboardWriter
│   ├── sync/        SyncManager / WebSocketClient / WebSocketServer / SyncProtocol
│   ├── ui/          UiDispatcher / MainWindow / SettingsWindow
│   ├── tray/        TrayIcon（托盘图标与菜单）
│   └── system/      StartupManager（开机启动注册表）
├── resources/       icon.ico / resource.rc（编入 EXE）
├── third_party/
│   └── sqlite/      sqlite3 amalgamation（静态编译）
└── build/           CMake 构建目录（生成 ClipLink.exe）
```

## 编译方法

依赖：CMake ≥ 3.16、MinGW-w64 g++（支持 C++17）、windres（RC 资源编译）。

**Release（发布）**

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**Debug（调试）**

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

产物：`build\ClipLink.exe`。全程静态链接（`-static`），单 EXE、
无外部运行时 DLL 依赖；编译告警级别 `-Wall -Wextra`，sqlite3 第三方源码
以 `-w` 抑制（其自带告警），最终要求 **0 error 0 warning**。

## 运行说明

1. 双击 `ClipLink.exe`。
2. 首次启动自动创建 `%APPDATA%\ClipLink\`（配置 / 数据库 / 日志）与
   持久化 `deviceId`。
3. 程序默认**不弹主窗口**，驻留系统托盘：
   - 左键 / 双击托盘图标 → 打开主窗口（200 × 500，剪贴板历史列表）
   - 右键 → 菜单：打开主界面 / 暂停同步 / 设置 / 启动服务 / 退出
   - 主窗口右上角 ⚙ → 打开设置窗口
   - 主窗口点 X → 隐藏到托盘（不退出）
4. 复制任意文本即入历史；点击历史项即写回系统剪贴板（不产生同步事件）。

## 启动 Server（服务端）

程序本身内置 WebSocket Server，两种典型拓扑：

- **单机自测**：设置页勾选「启动服务」，端口保持 `9000`，保存。程序监听
  `0.0.0.0:9000`，并作为 Client 连接 `ws://127.0.0.1:9000`，即可自收自发。
- **局域网多设备**：在 PC-A 上勾选「启动服务」并保存；PC-B / Android 在
  设置页把服务器地址填为 `ws://PC-A的局域网IP:9000` 后保存。

端口冲突不会崩溃：保存时会给出错误提示（托盘「启动服务」同理），更换端口
后重试即可。托盘菜单「启动服务」的勾选态反映当前服务启停状态。

## 连接客户端（服务端模式）

1. 在服务端 PC 的设置页勾选「启动服务」→ 保存。
2. 确认防火墙放行该端口的入站 TCP（仅建议放行专用网络/局域网配置文件）。
3. 在客户端 PC 的设置页填写服务器地址 `ws://<服务端IP>:9000` → 保存，
   程序自动连接、注册设备并上传待同步事件、按 `lastServerSeq` 拉取历史。
4. 连接状态在主窗口与托盘菜单/Tooltip 显示：已连接 / 连接中 / 断开。

## 端口配置

- 设置页「服务端口」：Server 监听端口（默认 9000，范围 1–65535）。
- 设置页「服务器地址」：客户端连接目标，支持 `ws://` 与 `wss://`
  （MVP 以 `ws://` 局域网为主），端口写在地址中。
- 修改后点「保存」即生效（网络相关变更会自动重启同步）。

## 数据库 / 配置 / 日志位置

首次启动自动创建（均在当前用户 `APPDATA` 下，不需要管理员权限）：

| 内容 | 路径 |
| --- | --- |
| 配置 | `%APPDATA%\ClipLink\config.json` |
| 数据库 | `%APPDATA%\ClipLink\cliplink.db`（SQLite，`clipboard_history` 表） |
| 日志 | `%APPDATA%\ClipLink\logs\cliplink.log`（5MB 滚动为 `.log.1`） |

配置项包括：`deviceId` / `deviceName` / `serverAddress` / `serverEnabled`
（启动服务）/ `port` / `autoStart`（开机自启）/ `syncEnabled`（自动同步）/
`maxHistoryCount` / `maxHistoryDays`。

> 卸载/删除程序时：删除 `%APPDATA%\ClipLink\` 目录，并在设置页关闭
> 「开机自动启动」（或手动删除注册表
> `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下的 `ClipLink` 值）。

## 开机自动启动

设置页勾选「☐ 开机自动启动」→ 保存。实现方式为 Windows 官方推荐的
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`：
仅当前用户、无需管理员权限、可随时关闭、删除程序时可清理（§5）。

## 单 EXE 发布

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
copy build\ClipLink.exe <发布目录>\
```

`ClipLink.exe` 静态链接全部依赖，双击即可运行，无安装程序、无运行时 DLL。

## 移动端页面联调

`mobile-web\index.html` 是 ClipLink 同步协议的**手机端界面预览页**
（单文件：内联 CSS/JS、零外部依赖、UTF-8、中文界面；桌面打开时以约
375px 宽度居中显示为手机外形），用于在真实 Android 应用就绪前做联调
演示，**非真实 Android 客户端**。

页面实现与 `src/sync/SyncProtocol.h` 完全一致的 `{"type","data"}` 信封协议：

- 连接成功 / 重连后发送 `register`（`deviceId` + `deviceType="ANDROID"` +
  `deviceName`），随后发送 `sync_request(lastServerSeq)` 补历史；
- 点击「发送」构造 `clipboard` 消息（`eventId` 用 UUID、`contentHash` 为
  简单哈希（双路 FNV-1a，联调用实现）、`clientTime` 为 UTC Unix 毫秒、
  `contentType="TEXT"`）；
- 收到 `clipboard` 插入历史列表；收到 `clipboard_ack` 记录并持久化
  `serverSeq`；收到 `sync_response` 按 `serverSeq` 增量补入历史；
- `deviceId`（`crypto.randomUUID()`）与 `lastServerSeq` 均持久化到
  `localStorage`；断线自动重连按 1/2/5/10/30 秒退避（30 秒封顶，连接
  成功后复位）。

### 启动 ClipLink.exe 服务

1. 按上文「编译方法」构建，双击运行 `build\ClipLink.exe`（程序驻留托盘）。
2. 打开设置（主窗口右上 ⚙ 或托盘菜单「设置」），勾选「启动服务」，
   服务端口保持 `9000`，点「保存」。服务监听 `0.0.0.0:9000`；
   也可用托盘菜单「启动服务」直接启停。
3. 确认防火墙放行该端口入站 TCP（仅建议专用网络/局域网配置文件）。

### 用本页面连接联调

1. 双击或用浏览器打开 `mobile-web\index.html`（页面加载后自动发起首次
   连接；`file://` 页面发起 `ws://` 不受 CORS 限制）。
2. 「服务器地址」默认 `ws://127.0.0.1:9000`（单机自测）；联调局域网
   服务端时改为 `ws://<服务端IP>:9000`，点「连接」。
3. 连接成功后顶部状态显示「已连接」，页面日志区会显示
   `register` / `sync_request` 发送记录。
4. 联调验证点：
   - 页面输入文本点「发送」→ PC 端 ClipLink 应实时收到该剪贴板内容；
   - PC 端复制文本 → 页面「历史记录」应实时新增该条（`clipboard` 下行）；
   - 页面发送后日志出现 `clipboard_ack：... serverSeq=N`，刷新页面后
     `lastServerSeq` 从 N 继续（localStorage 持久化生效）；
   - 断开服务端再恢复 → 页面按 1/2/5/10/30 秒退避重连，重连成功后自动
     `sync_request` 补齐缺口历史；
   - 勾选「暂停同步」→ 页面不发送、不接收；取消勾选后自动补拉历史。
5. 页面点击历史条目会把内容送到顶部预览区（预览演示行为，不写系统剪贴板）。

### 页面语法校验（命令与结果）

```powershell
# 1) Python：HTML 标签配对 + charset/DOCTYPE 检查，并提取 <script> 内容
#    （检查脚本：读取 index.html，正则配对标签，输出报告到 temp 目录）
python <检查脚本>   # 结果见下方
# 2) Node：对提取出的 JS 做语法校验
node --check temp\mobile_index_script.js
node --version
```

校验结果（2026-09-29）：

| 检查项 | 结果 |
| --- | --- |
| HTML 结构（python 标签配对 / charset / DOCTYPE） | 通过，tag balance errors = 0 |
| JS 语法（`node --check`，Node v20.15.0） | 通过，exit code 0 |
| 协议字段完整性（register / clipboard / clipboard_ack / sync_request / sync_response / ANDROID / eventId / contentHash / clientTime / lastServerSeq / 退避数组 / 默认地址等 21 项） | 全部 PASS |
| 外部依赖（`http://` 引用扫描） | 无外部依赖 |

## 安全说明（重要）

- MVP 仅面向**局域网**使用：默认监听本地/内网，不做端口映射、不做公网穿透、
  不连接任何第三方云服务器。
- **禁止将服务端端口直接暴露公网**。剪贴板内容可能包含密码、Token、
  Cookie、证件号、银行卡号等敏感信息，任何公网暴露都可能造成泄露（§70）。
- 需要跨公网访问时，请自行通过受信任的 VPN / 隧道方案接入，不要直接映射端口。

## 已知边界（MVP 范围）

- 仅支持 Unicode 文本（`CF_UNICODETEXT`，单条上限 1MB），图片/文件/HTML/RTF
  不在本期范围（§10 / §73）。
- 不做剪贴板搜索、分类、收藏、账号体系等（§89）。
*（内容由AI生成，仅供参考）*

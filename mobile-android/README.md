---
AIGC:
    Label: "1"
    ContentProducer: 001191440300708461136T1XGW3
    ProduceID: 58e87cd5ac29bab7322874bd6b207f1c_38216bb1bbca11f1a526525400cd780f
    ReservedCode1: xhdaIL4AwzqokfqoPIEl//Ej5KAwGMuERZjeHDJkf73kB1EiQlcmuNeYbl9hdaERamoOgPaotheW166nv6cS0gl+WEl1IWqdC6SVg7ckpCgPHqr3ifrHoRSO9MCB1w34XUkpJmyhEDdj1OOimh5XdRJsIH4jOsSk6WqvG5p87eSCJ0vzimTOZcalEWE=
    ContentPropagator: 001191440300708461136T1XGW3
    PropagateID: 58e87cd5ac29bab7322874bd6b207f1c_38216bb1bbca11f1a526525400cd780f
    ReservedCode2: xhdaIL4AwzqokfqoPIEl//Ej5KAwGMuERZjeHDJkf73kB1EiQlcmuNeYbl9hdaERamoOgPaotheW166nv6cS0gl+WEl1IWqdC6SVg7ckpCgPHqr3ifrHoRSO9MCB1w34XUkpJmyhEDdj1OOimh5XdRJsIH4jOsSk6WqvG5p87eSCJ0vzimTOZcalEWE=
---

# ClipLink Android 客户端（mobile-android）

纯 Java 实现的 ClipLink Android 端（包名 `com.cliplink.mobile`），与 PC 端
（`ClipLink.exe`，C++ Win32）通过 WebSocket 同步剪贴板，本地 SQLite 保存历史。

- 技术栈：原生 View / LinearLayout / RecyclerView（无 Compose、无第三方 UI 库）
- WebSocket：`org.java-websocket:Java-WebSocket`；`org.json` 为平台自带
- minSdk 24 / targetSdk 34 / compileSdk 34 / Java 17
- AGP 8.7.3 + Gradle 8.9（wrapper 已固定 gradle-8.9-bin.zip）

## 一、构建方法

### 1. 环境要求

- JDK 17（本机路径：`D:\01App\01dev\zip_jdk\jdk-17.0.20.1+1`）
  已在 `gradle.properties` 通过 `org.gradle.java.home` 固定，无需设置系统 JAVA_HOME
- Android SDK（本机路径：`D:\01App\01dev\Android\Sdk`）

### 2. local.properties

工程根目录 `mobile-android/local.properties` 中配置 SDK 路径（Windows 反斜杠需转义或用正斜杠）：

```properties
sdk.dir=D:/01App/01dev/Android/Sdk
```

### 3. 构建 Debug 包

在 `mobile-android` 目录执行：

```powershell
cd D:\02Code\06Ai_project\ClipLinkPC\mobile-android
.\gradlew.bat assembleDebug --console=plain
```

产物：`app\build\outputs\apk\debug\app-debug.apk`

首次构建需联网下载依赖（AGP / AndroidX / Java-WebSocket）。构建成功输出
`BUILD SUCCESSFUL`，0 error。

## 二、与 PC 端联调步骤

1. **PC 启动服务**：运行 `ClipLink.exe`，确认服务端 WebSocket 监听 `0.0.0.0:9000`
   （端口可在 PC 端设置中修改）。
2. **同一局域网**：手机与 PC 接入同一局域网（Wi-Fi），关闭 PC 防火墙对 9000 端口的拦截，或放行 ClipLink。
3. **手机配置**：安装并打开 App，在「连接配置」中填写：
   - 服务器地址：PC 的局域网 IP（如 `192.168.1.100`；也支持完整 `ws://192.168.1.100:9000`）
   - 端口：`9000`（与 PC 一致）
   - 设备名称：自定义显示名（默认为手机型号）
4. **保存并重连**：点击「保存并重连」，顶部状态条变为绿色「已连接」即成功；
   也可直接点击状态条手动重连。
5. **同步验证**：
   - PC 复制文本 -> 手机系统剪贴板自动更新，手机历史列表新增一条（已同步）
   - 手机复制文本 -> PC 剪贴板更新，服务器分配 serverSeq
   - 手机历史列表点击任意条目 -> 仅写入系统剪贴板，不产生新同步事件
   - 「暂停同步」期间仍保存本地历史，恢复后自动补传/补拉

> 注意：MVP 仅限局域网使用，禁止将服务端口直接暴露公网（剪贴板可能含敏感信息）。

## 三、协议字段简述

所有消息统一 `{ "type": "...", "data": {} }` 结构（禁止字符串硬匹配）。

| type | 方向 | 关键字段 |
| --- | --- | --- |
| `register` | 客户端 -> 服务器 | `deviceId`、`deviceType`、`deviceName` |
| `clipboard` | 客户端 -> 服务器 | `eventId`(UUID)、`deviceId`、`deviceType`、`deviceName`、`contentType`(TEXT)、`contentHash`(SHA-256)、`content`、`clientTime`(UTC 毫秒) |
| `clipboard_ack` | 服务器 -> 客户端 | `eventId`、`serverSeq`、`serverTime` |
| `clipboard` | 服务器 -> 客户端（广播，不含来源设备） | 上述字段 + `serverSeq` |
| `sync_request` | 客户端 -> 服务器 | `lastServerSeq`（增量补历史游标） |
| `sync_response` | 服务器 -> 客户端 | `events[]`（server_seq > lastServerSeq 升序） |
| `error` | 服务器 -> 客户端 | 错误描述 |

同步机制要点：

- `eventId` UUID 幂等防重复；`contentHash` SHA-256 防内容重复与同步循环
- `serverSeq` 服务器全局单调递增，是唯一顺序依据（不依赖客户端时间）
- `lastServerSeq` 离线增量同步；补历史只入库，不覆盖当前剪贴板
- 来源分类 `USER / REMOTE / HISTORY`：程序写入剪贴板不再回发（防循环双保护）
- 历史点击（HISTORY）只写剪贴板，不入库、不发送

## 四、工程结构

```
mobile-android/
├── app/src/main/java/com/cliplink/mobile/
│   ├── MainActivity.java          # 单主界面（状态条/暂停/配置/历史列表）
│   ├── HistoryAdapter.java        # 历史 RecyclerView 适配器
│   ├── ClipLinkApp.java           # Application：配置与数据库初始化
│   ├── protocol/                  # 协议层：SyncProtocol/ClipboardEvent/HashUtil/UuidUtil
│   ├── data/                      # 数据层：DatabaseHelper/ClipboardRepository/ConfigRepository
│   ├── clipboard/                 # 剪贴板：ClipboardHelper（监听/读写/防循环）
│   ├── sync/                      # 同步层：SyncManager/WebSocketClientWrapper
│   └── service/                   # SyncForegroundService 前台服务
├── app/src/main/res/              # 布局/文案/颜色/vector 图标
├── gradle.properties              # JDK17 路径、Gradle JVM 参数
├── local.properties               # sdk.dir（本地生成，不入库）
└── README.md
```
*（内容由AI生成，仅供参考）*

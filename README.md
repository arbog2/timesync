# LAN Time Sync（局域网时钟同步）

Windows 下的局域网强制时钟同步工具（SNTP over UDP），纯 C 编写、无第三方依赖、单文件 exe。
主要面向**学校机房 / 考场**场景：一台教师机做时间源，学生机自动把系统时钟校准到它。

## 功能

- **托盘常驻 + 图形界面**：状态（偏移 / RTT / 上次同步 / 下次同步）一目了然，可随时点「立即同步」。
- **Windows 服务模式**：安装为系统服务后以 SYSTEM 权限在后台持续校时，不用登录、不用 UAC。
- **非管理员也能同步**：托盘模式下点「立即同步」会自动走 UAC 提权（`--once` 子进程），结果回显到界面状态栏，不弹窗打扰。
- **本机时钟服务**：一键开启 W32Time NTP Server 并放行防火墙 UDP 123，本机即可充当时间源供他人同步。
- **开机自启动**：计划任务方式（最高权限），Explorer 没就绪时托盘图标自动重试。
- **密码保护**：从托盘打开主窗口 / 退出时可要求输密码，防学生乱改乱退。
- **配置完整性守卫**：`config.ini` 被外部篡改（改成无效服务器、删文件等）时自动恢复到上次能同步成功的值。
- **互相看门狗**：服务与托盘进程互相看护，被杀后自动拉回；用户亲手停的服务不会被拉起。
- **桌面水印**：默认在桌面显示本机机器名，学生在机房一眼知道自己坐在哪台机器上（可关）。
- **日志**：默认写 `C:\ProgramData\LANTimeSync\time_sync.log`，等级可在配置里调。

## 快速开始

1. 到 [Releases](../../releases) 下载 `timesync.exe`（x64）或 `timesync_x86.exe`（x86），放到任意目录（推荐与 `config.ini` 同目录， portable 运行）。
2. 双击运行，在「服务器地址」里填时间源 IP（如教师机 `192.168.1.100`），端口一般 `123`。
3. 点「立即同步」（首次会弹 UAC，点「是」）。
4. （可选）点「安装为系统服务」，以后开机即在后台自动校时，GUI 都不用开。
5. （可选）教师机点「开启时钟服务」，本机就成为局域网时间源。

配置文件默认在 `%ProgramData%\LANTimeSync\config.ini`；若 exe 同目录下有 `config.ini` 也会优先读取它（便携模式）。

## 配置说明

示例（`config-test/config.ini` 是一份可直接改的模板）：

```ini
[server]
address = "192.168.1.100"   ; 时间源 IP 或主机名
port = 123

[sync]
interval_minutes = 60       ; 常规重同步周期（分钟）
retry_seconds = 10          ; 上次失败后多久重试（秒）
timeout_ms = 3000           ; 每次 UDP 请求超时（毫秒）
max_offset_ms = 5000        ; 偏差超过它才校正
min_offset_ms = 200         ; 低于它视为已准，不写时钟
force_enabled = true        ; 校时总开关
force_flag = true           ; true=激进模式，小幅漂移也校
sync_on_start = true        ; 启动即同步一次
allow_large_offset = true   ; true=允许跨年式大跳变

[service]
enabled = false             ; true=以 Windows 服务方式运行

[log]
file_enabled = true
file_path = "C:\\ProgramData\\LANTimeSync\\time_sync.log"
level = "info"              ; debug/info/warn/error

[watermark]
enabled = true              ; 桌面水印开关
text = ""                   ; 空=显示机器名；可用 %COMPUTERNAME% 做模板
```

界面上的「应用并保存」会写回该文件；也可用「用记事本编辑」直接改。

## 命令行

```text
timesync                      托盘常驻运行（按配置决定是否走服务）
timesync --install-service     安装为 Windows 服务并启动
timesync --uninstall-service   卸载服务
timesync --service-start       启动服务
timesync --service-stop        停止服务
timesync --once                只同步一次并退出（供提权调用）
timesync --config <file>       指定配置文件
timesync --tray                启动时只进托盘，不弹主窗口
timesync --help                帮助
```

## 自己编译

需要 Visual Studio C++ 工作负载（含 MSVC）+ Windows SDK 10，目标 Win7 SP1 及以上：

```bat
build.bat        :: 编译 x64，输出 bin\timesync.exe
build_x86.bat    :: 编译 x86，输出 bin\timesync_x86.exe
build_all.bat    :: 两个都编
```

脚本会自动定位本机最新的 MSVC 工具集与 SDK，无需手动配环境变量。

`tools/localntp.c` 是一个本地测试用 SNTP 服务器（已附 `tools/localntp.exe`），联调时可用它冒充时间源。

## 目录结构

```text
src/            全部源码（C，无第三方依赖）
  main.c        入口、命令行分发、托盘/服务两种运行模式
  window.c      托盘 GUI 主窗口
  task.c        同步 worker（定时 + 立即同步）
  ntp.c         SNTP 客户端（UDP，最多 3 次采样取最优）
  timeset.c     写系统时钟 + 提权
  service.c     Windows 服务安装/启停/查询
  ntpserver.c   本机 W32Time NTP Server 开关 + 防火墙放行
  config.c      config.ini 解析/保存/默认值
  cguard.c      配置完整性守卫（防篡改回滚）
  trayguard.c   反向看门狗（服务拉回托盘）
  startup.c     开机自启动（计划任务）
  auth.c        密码保护
  watermark.c   桌面水印
  util.c        日志、单实例、状态锁等基础件
tools/          localntp 本地测试 SNTP 服务器
config-test/    示例配置模板
bin/            编译产物（release 的 exe 也在这里归档）
```

## 系统要求

- Windows 7 SP1 及以上（x64 / x86 均有对应版本）。
- 改系统时间需要管理员权限：服务模式自带 SYSTEM 权限；托盘模式首次同步走一次 UAC 即可。

# debug-stackchan-142-upgrade

Status: [PARTIAL VERIFIED]

## Symptoms

- StackChan 开源主线已更新到 1.4.2，设备侧自动固件 OTA 会写入整包 firmware。
- 本 fork 在首页拆分了 `VeRTC.Agent` 与 `AI.Agent`，整包 OTA 会覆盖本地新增的 VeRTC/Volcengine 链路。

## Hypotheses

- 只升级 StackChan/Xiaozhi 原生链路相关代码，不能照搬会绕过首页或覆盖双链路入口的启动逻辑。
- Xiaozhi 版本检查可以保留，但固件下载、写 OTA 分区、重启切分区必须禁用。
- 未来升级应走手动拉取上游、比对、拷贝/移植、编译、烧录流程。

## Evidence

- `m5stack/StackChan` 最新 main：`fd09a7441a5c2b372cefd35600a03ff5bd64a2ea`
  (`Merge pull request #89 from Forairaaaaa/v1.4.2-firmware-update`)。
- 2026-06-16 复核：`git ls-remote https://github.com/m5stack/StackChan.git HEAD refs/heads/main`
  仍指向 `fd09a7441a5c2b372cefd35600a03ff5bd64a2ea`；官方 GitHub commit 页面显示该提交为
  `update firmware v1.4.2`，仓库未发布 GitHub Release。
- 最新 `firmware/main/main.cpp` 新增 `skip_mooncake`，可在 `startAiAgentOnBoot` 时跳过 Mooncake 首页并直接启动 Xiaozhi。
  本 fork 不能接入该行为，否则会绕过 `VeRTC.Agent` / `AI.Agent` 首页选择。
- 最新 motion 更新把角度计算拆为 `motion_math.*`，不触碰 Volcengine RTC 网络、音频、工具调用路径。
- 本 fork 已有 `motion_math.*` 与 `Servo::stop_motion_at_angle()`，但 2026-06-16 复核发现仍缺
  `PROJECT_VER=1.4.2`、头像点击 2 秒防抖、pitch 舵机堵转保护。
- `firmware/xiaozhi-esp32/main/application.cc` 的 `CheckNewVersion()` 原本在发现新固件时调用 `UpgradeFirmware()`。
- `firmware/xiaozhi-esp32/main/mcp_server.cc` 原本注册 `self.upgrade_firmware`，模型/用户工具可触发固件 OTA。
- `firmware/main/hal/hal_ota.cpp` 的系统更新入口原本调用 `Ota::Upgrade()` 写 OTA 分区。

## Fix

- 保留 Xiaozhi/StackChan 固件版本检查，但发现新版本时只记录/提示“需要手动升级”，不下载、不写 OTA 分区、不重启。
- 删除 `Application::UpgradeFirmware()` 与 `self.upgrade_firmware` 工具注册。
- `Hal::updateFirmware()` 发现新固件时返回成功提示，但不调用 `Ota::Upgrade()`。
- 同步开源 1.4.2 motion 数学拆分：新增 `motion_math.*`，并同步 `Servo::stop_motion_at_angle()`。
- 补齐开源 1.4.2 固件版本号 `PROJECT_VER=1.4.2`。
- 补齐开源 1.4.2 头像点击 2 秒防抖，并保留本 fork 的 VeRTC 运行中点击 interrupt 逻辑。
- 补齐开源 1.4.2 pitch 舵机堵转保护；`hal_servo.cpp` 与 upstream 仅保留格式差异。
- 重新基于 `78/xiaozhi-esp32@v2.2.4` 生成 `firmware/patches/xiaozhi-esp32.patch`。
- 在 `AGENTS.md` 写入设备侧 OTA 禁用规则。

## Verification

- `git diff --check -- . ':!firmware/patches/xiaozhi-esp32.patch'` 通过。
- `firmware/patches/xiaozhi-esp32.patch` 在干净 `78/xiaozhi-esp32@v2.2.4` 上 `git apply --check` 通过。
- 首次 `idf.py build` 因新增 `motion_math.cpp` 未进入旧 CMake 缓存而链接失败；执行 `idf.py reconfigure build` 后通过。
- `Ota::Upgrade` 只剩 `firmware/xiaozhi-esp32/main/ota.cc` 定义，AI.Agent 启动、MCP 工具、About 页更新入口均不再调用。
- 未烧录：`/dev/cu.usbmodem2101` 被 Claude 视觉采集进程 PID 67539 占用，写入 `/tmp/stackchan_vision.log`。
- 2026-06-16 `idf.py reconfigure build` 通过；构建日志显示 `App "stackchan-volc-open" version: 1.4.2`。
- 2026-06-16 `/dev/cu.usbmodem2101` 当前不存在，设备枚举为 `/dev/cu.usbmodem101`；`lsof` 确认空闲后，
  `idf.py -p /dev/cu.usbmodem101 build flash` 通过，app 与 assets 分区写入并 hash 校验成功。
- 2026-06-16 串口复位启动确认 `Project name: stackchan-volc-open`、`App version: 1.4.2`，首页仍创建
  `VeRTC.Agent` 后创建 `AI.Agent`。
- 2026-06-16 串口点击验证中，`AI.Agent` 可进入 Xiaozhi 原生链路，音频 codec、MCP 工具注册、网络与
  OTA 检查启动；日志显示 `Ota: Current version: 1.4.2` 与 `Ota: Current is the latest version`，未触发 OTA 写分区。
- 2026-06-16 独立 worktree 首次未带 `firmware/sdkconfig.local` 时，`VeRTC.Agent` 正常打开但因缺少本地私密配置
  报 `missing volc config` 并返回首页；同步 ignored 的本地 `sdkconfig.local` 后重新构建烧录，启动与首页验证通过。
- 2026-06-16 启动日志仍有既有首页/status bar 图标资源缺失报错；相关资源不在本次 1.4.2 升级 diff 中，本轮未扩大范围修复。
- 待执行：屏幕点击与真实对话场景确认 VeRTC.Agent 完整会话、AI.Agent 唤醒/对话体感正常后，再合并回 main 并删除分支。

## Resolution

StackChan/Xiaozhi 原生链路的设备侧 OTA 已改为“只检查/提示，手动代码升级”。后续同步开源版本时必须拉取上游源码做文件级比对与拷贝/移植，再本地编译、烧录；不得让设备直接下载并写入上游整包固件，否则会覆盖 `VeRTC.Agent` 链路。

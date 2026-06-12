# WORKLOG — 会话工作日志（新条目加在最上面）

> 每个 AI 会话开工时在此登记，收工时更新结果。格式：
> `## YYYY-MM-DD [工具] 任务一句话` + 占用域 + 结果/遗留。

## 2026-06-12 [claude] VeRTC 视觉采集 + 工具执行层实测修复（音量崩溃两层根因）

- 占用域：firmware-audio + cores3_audio_codec
- 视觉采集（已验证）：2s/帧 MJPEG 经 RTC 发送，实测 jpeg≈4KB、enc=95~222ms、
  sent=true、内部 SRAM 稳定 35KB——低频采集无性能问题（commit dc1f28b）
- 工具链路首轮真实流量（云端投递已通）：set_head_angles/shake_head 参数与执行全通
- 音量工具崩溃（已修复+用户实测通过）：
  1. SetOutputVolume 的 ESP_ERROR_CHECK 在扬声器未打开时 abort 重启
  2. 基类 NVS 持久化是 flash 写，要求内部栈；volc_tool 是 PSRAM 栈 →
     esp_task_stack_is_sane 断言重启（"调音量退回主界面"的真因）
  修复：硬件音量降级告警 + 持久化按调用方栈位置分流（PSRAM 栈 defer 到
  esp_timer 任务执行），esp_timer 栈 3584→8192（sdkconfig.defaults 同步）
- 遗留：无

## 2026-06-12 [codex] StackChan 原生链路对齐 1.4.2 + 禁止自动 OTA 覆盖 VeRTC

- 占用域：firmware-ui + firmware-audio + sdk-patches + docs-cloud
- 计划：对比 m5stack/StackChan 最新开源版本与本地原生链路；把 Xiaozhi/StackChan 系统自动升级改为手动拉取/拷贝升级流程；保护 VeRTC/Volcengine 链路不被 OTA 覆盖；验证并记录 AI.Agent 与 VeRTC.Agent 回归
- 结果：已对比 m5stack/StackChan main `fd09a744`（v1.4.2 firmware update），未接入会绕过首页的 `skip_mooncake`；
  已禁用 Xiaozhi `CheckNewVersion()` 自动固件 OTA、移除 `self.upgrade_firmware`，并把 About 页系统更新改为只提示手动升级；
  已同步 1.4.2 motion 数学拆分，更新 `firmware/patches/xiaozhi-esp32.patch` 与 `AGENTS.md` OTA 禁用规则；
  `git apply --check firmware/patches/xiaozhi-esp32.patch`（基于 `78/xiaozhi-esp32@v2.2.4`）通过，`idf.py reconfigure build` 通过
- 遗留：未烧录；`/dev/cu.usbmodem2101` 被 Claude 视觉采集进程 PID 67539 占用（写 `/tmp/stackchan_vision.log`），本会话未终止他人采集

## 2026-06-12 [codex] 同步 StackChan 原生升级隔离规则并重新烧录

- 占用域：docs-cloud
- 计划：在项目规则中补充 StackChan 原生链路升级不得影响 VeRTC/Volcengine 链路，并重新烧录当前固件
- 结果：已在 AGENTS.md 补充 StackChan/Xiaozhi 原生升级隔离规则和升级信息同步要求；
  `idf.py -p /dev/cu.usbmodem2101 build flash` 通过，应用与 assets 分区写入校验成功并硬复位；
  复位后串口有 SystemInfo 正常运行输出
- 遗留：未做 AI.Agent / VeRTC.Agent 交互回归，本次只同步规则并重新烧录当前固件

## 2026-06-12 [codex] 调整首页入口顺序：VeRTC.Agent 在 AI.Agent 前

- 占用域：firmware-ui
- 计划：调整 App 安装顺序，让首页第一个业务入口为 VeRTC.Agent，AI.Agent 排在后面
- 结果：已调整 main.cpp 安装顺序；Launcher 会移除自身并保留业务 App 安装顺序，因此首页第一个业务入口为 VeRTC.Agent，AI.Agent 排在后面；idf.py build 通过
- 遗留：未烧录做屏幕目视确认

## 2026-06-12 [codex] 首页拆分 AI.Agent / VeRTC.Agent + 多 AI 项目规则补充

- 占用域：firmware-ui + firmware-audio + docs-cloud
- 计划：更新协作规则；把首页入口拆成原生 StackChan AI.Agent 与 Volc VeRTC.Agent；验证后合回 main 并删除临时分支
- 结果：已拆成首页两个入口；AI.Agent 只请求原生 StackChan/Xiaozhi 链路，VeRTC.Agent 只启动 Volc 链路；
  新增 VeRTC 首页背景图；已更新 AGENTS.md / Trae 规则指针；idf.py build 与实机 flash 通过，
  串口验证 VeRTC listening、tool call、tool result、bot 音频链路正常
- 遗留：AI.Agent 原生链路本次保留原请求路径并通过编译验证，未在串口中单独打开回归

## 2026-06-12 [claude] volcRTC 本地工具调用（RTS 协议）+ 字幕显示调用过程 + 唤醒前声源定位对齐

- 占用域：firmware-audio（volc_agent.cpp）+ docs-cloud（服务端 tools 配置梳理）
- 结果：
  - volc RTS 工具协议对照官方 function_call_service 逐字段核验一致（tool 下发/func 回包），新增 info magic（function_calling 触发事件）处理
  - 工具调用过程三阶段上字幕：[工具] xxx 调用中/执行中/完成（volc + xiaozhi 两侧一致）
  - xiaozhi MCP 补齐 shake_head（参数较 volc 保守，回调跑在主循环）；音量用 xiaozhi 内建工具
  - 唤醒前声源定位核对确认 volc 已具备（待机 DOA + 唤醒转头），无需新增
  - 新增 docs/CLOUD_TOOLS_CONFIG.md：9 工具 schema + FunctionCallingConfig 投递配置 + 验证清单
  - 编译烧录通过，启动日志确认 shake_head 注册成功
- 遗留：等用户配置云端 FunctionCallingConfig（客户端投递）+ 9 工具 schema 后联调；
  验证清单见 docs/CLOUD_TOOLS_CONFIG.md §5

## 2026-06-12 [claude] 工具不执行排查 + 执行层加固

- 占用域：firmware-audio
- 结论：云端配置后设备收到 info 触发事件（字幕正常），但 tool_calls 本体仍未下发
  （无一条 tool call: 日志），LLM 参数泄漏进 TTS（"speed。"/"6。"）——云端当前为
  "触发通知模式"，需改为"客户端工具执行模式"→ debug-llm-no-reply-fc-config.md
- 端侧加固（已烧录）：arguments 字符串/对象兼容；set_led_color 工具接管灯色
  （修复状态机灯色瞬间覆盖工具颜色）+ 直写硬件；volc 启动对齐 xiaozhi 舵机使能；
  set_volume/led applied 日志；未知 magic 警告日志
- 遗留：云端模式整改后回归三件套（灯/音量/摇头）

## 2026-06-12 [claude] 建立多 AI 协作机制

- 占用域：docs-cloud
- 新增 AGENTS.md / WORKLOG.md / CLAUDE.md / .trae 规则指针
- 遗留：无

---

## 当前项目状态快照（2026-06-12）

### 已解决（详见对应 debug-*.md）

- ✅ 上行人声截断：根因 RTC opus RTP 参数误设 16000/320（正确 48000/960），已修复并验证
  长句 ASR 完整准确 → debug-uplink-stream-drop.md [RESOLVED]
- ✅ TTS 字幕缺字：根因内置 basic 字体仅 800 字形，改用 assets 完整字体
- ✅ 唤醒后重启 / 重入崩溃 / "engine is not in CREATED state"：SDK 生命周期 bug，
  patches/volc_conv_ai.patch 已修，上游 PR volcengine/ConversationalAI-Embedded-Kit-2.0#5
- ✅ 上行吞吐天花板（vTaskDelay(1)=10ms/帧）、WiFi 省电断流、_mutex 跨网络发送卡 UI

### 待办（按优先级）

1. **云端 FunctionCallingConfig 为空**（LLM 调工具后无回复的根因）：需在火山控制台把
   Function Call 投递方式配置为客户端投递（RTS 消息）→ debug-llm-no-reply-fc-config.md
2. **端侧工具链路回归**：云端投递打通后，回归 9 个 self.robot.* 工具
   （tool magic → volc_tool 任务 → func 回包，该链路从未被真实流量验证）
3. 云端断句配置：静音断句时长建议调大至 800ms+；WebSearch 插件疑似超时挂起待排查
4. volc_agent.cpp 已 ~2000 行，建议拆分 audio_pipeline / session / tools（开 refactor 分支）
5. docs/DESIGN.md 与实现漂移严重（AiBackendArbiter 等不存在），开源前需重写
6. 远期：AFE 软参考 AEC（把播放 PCM 喂回 AFE R 通道），实现全双工语音打断

### 设备与环境

- 设备：M5Stack CoreS3 × 1，串口 /dev/cu.usbmodem2101
- 工具链：ESP-IDF v5.5.2（source /Users/bytedance/esp-idf-v5.5.2/export.sh）
- 凭据：firmware/sdkconfig.local（不进 git）

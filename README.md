# StackChan Volcengine RTC

> 在 [M5Stack StackChan](https://github.com/m5stack/StackChan) 平台上提供**两套开箱即用的 AI 对话后端**，作为开源参考实现：
>
> - `AI.Agent`：基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 协议，适合自托管 / 私有部署
> - `VeRTC.Agent`：基于 [VolcEngine ConversationalAI Embedded Kit 2.0](https://github.com/volcengine/ConversationalAI-Embedded-Kit-2.0)，适合云端智能体编排、RTC 音视频与端侧工具调用
>
> 两个入口共享板级 HAL、Avatar/LED/Servo 表现层与 StackChan 机器人能力；首页中 `VeRTC.Agent` 在 `AI.Agent` 前。

## Highlights

- **双后端共存** — 同一固件，launcher 中选择 `VeRTC.Agent` 或 `AI.Agent`；任意时刻仅一个入口持有音频/摄像头资源。
- **板级 HAL 复用** — 板级代码独立 component，可被 ESP32 任意 AI 项目引用。
- **云端智能体编排** — `VeRTC.Agent` 暴露 9 个端侧工具（舵机 / 摇头 / LED / 提醒 / 音量），并启用 WebSearch 联网搜索、音乐播放等云端内置工具；控制台可自定义新增工具。云端必须用客户端投递 RTS 消息调用端侧工具。
- **工具调用可观测** — volcRTC 与 Xiaozhi 链路都会显示 `[工具] xxx 调用中/执行中/完成` 字幕，便于现场排查。
- **视觉链路** — 火山后端支持低频 MJPEG 视觉采集，经 RTC 发送给云端智能体。
- **禁止设备整包 OTA** — `AI.Agent` / Xiaozhi 只检查并提示新版本；同步 StackChan 开源更新必须走代码层手动移植，避免覆盖 `VeRTC.Agent` 链路。
- **零凭据仓库** — 所有 ProductKey/Secret/AccessKey 走 NVS 或 `sdkconfig.local`，仓库严禁明文。

## Quick Start

```bash
# 1. 克隆项目并拉取上游依赖
git clone https://github.com/dolphinai626/StackChan_VolcengineRTC.git
cd StackChan_VolcengineRTC
./scripts/fetch_repos.sh        # 拉取 xiaozhi-board-hal / xiaozhi-app / volc_conv_ai

# 2. 配置凭据（不进入 git）
cp firmware/sdkconfig.local.example firmware/sdkconfig.local
$EDITOR firmware/sdkconfig.local                     # 填入 BotID / AccessKey 等

# 3. 构建并烧录
source /Users/bytedance/esp-idf-v5.5.2/export.sh
cd firmware
idf.py set-target esp32s3
idf.py build flash monitor
```

## Architecture

```
                 Mooncake Launcher
                         │
        ┌────────────────┴────────────────┐
        │                                 │
  VeRTC.Agent                       AI.Agent
  AppAiAgent::Volcengine            AppAiAgent::StackChan
        │                                 │
  Volc RTC/RTS                      Xiaozhi/MCP
        │                                 │
        └──────── Shared HAL / UI / Robot ┘
                         │
               M5Stack CoreS3 / ESP32-S3
```

云端工具配置见 [docs/CLOUD_TOOLS_CONFIG.md](docs/CLOUD_TOOLS_CONFIG.md)。
落地手册见 [docs/PORTING.md](docs/PORTING.md)。

## Function Call Tools

`VeRTC.Agent` 通过火山智能体 Tools 配置暴露 9 个端侧能力：

| Tool                         | 描述                       |
| ---------------------------- | -------------------------- |
| `self.robot.get_head_angles` | 读舵机当前 yaw/pitch       |
| `self.robot.set_head_angles` | 设置舵机目标位置（带速度） |
| `self.robot.shake_head`      | 左右摇头                   |
| `self.robot.set_led_color`   | 设置左右 NeonLight 颜色    |
| `self.robot.create_reminder` | 创建定时提醒               |
| `self.robot.get_reminders`   | 获取未完成提醒列表         |
| `self.robot.stop_reminder`   | 取消提醒                   |
| `self.robot.get_volume`      | 读取扬声器音量             |
| `self.robot.set_volume`      | 设置扬声器音量             |

火山链路必须把 Function Call 投递方式配置为**客户端投递（RTS 房间消息）**：
服务端下发 `tool` 消息，设备执行后用 `func` 回包。完整 JSON schema、System Prompt 模板与验证清单见 [docs/CLOUD_TOOLS_CONFIG.md](docs/CLOUD_TOOLS_CONFIG.md)。

`AI.Agent` / Xiaozhi 链路通过 MCP 自动注册同名机器人能力；音量使用 Xiaozhi 内建 `self.get_device_status` / `self.audio_speaker.set_volume`。

### 云端内置工具

除端侧工具外，火山控制台智能体还启用了两个由云端执行的内置工具（端侧无需实现，不显示 `[工具]` 字幕）：

| 内置工具 | 说明 |
| --- | --- |
| `WebSearch` | 联网搜索时效性信息（天气 / 新闻 / 股价 / 最新政策等）；模型能直接回答时不调用 |
| `music_player`（MusicAgent） | 用户明确要求播放或控制音乐时触发；模糊的"暂停/停止"不触发 |

### 在控制台自定义新增工具

火山控制台智能体支持自定义新增工具：**端侧执行的工具**需在智能体 Tools 里按 JSON schema 新增、保证客户端投递，并在 `volc_agent.cpp` 的 `dispatchTool` 实现同名分支（端侧实现 / docs / README 三处同步）；**云端内置 / 托管工具**（如 WebSearch、MusicAgent）只需在控制台对应 Config 开关启用。详见 [docs/CLOUD_TOOLS_CONFIG.md](docs/CLOUD_TOOLS_CONFIG.md)。

## Troubleshooting

- 工具不执行、LLM 调工具后无回复：优先检查云端 FunctionCallingConfig 是否为客户端投递。若串口只有 `function_calling triggered:`，没有 `tool call:`，说明只收到了触发通知，工具本体没有下发。详见 [debug-llm-no-reply-fc-config.md](debug-llm-no-reply-fc-config.md)。
- 上行 ASR 截断或长句丢失：RTC Opus RTP 参数必须保持 `48000/960`，端侧编码输入仍是 16k PCM。详见 [debug-uplink-stream-drop.md](debug-uplink-stream-drop.md)。
- `VeRTC.Agent` 后进入 `AI.Agent` 卡网络，或 Xiaozhi 播放卡顿：已修复 WiFi 已连接路径事件补发与 Opus 单包多帧解码。详见 [debug-stackchan-playback-network.md](debug-stackchan-playback-network.md)。

## Hardware

- M5Stack CoreS3（默认）
- 任何已适配本仓库板级 HAL 的 ESP32-S3 板（理论可移植）

## License

Apache-2.0. See [LICENSE](LICENSE).

第三方组件版权另见 [THIRD_PARTY_NOTICE.md](THIRD_PARTY_NOTICE.md)。

## Acknowledgments

- [m5stack/StackChan](https://github.com/m5stack/StackChan) — 原硬件 + 表情/舵机表达框架
- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — Xiaozhi 协议与板级 HAL 基础
- [volcengine/ConversationalAI-Embedded-Kit-2.0](https://github.com/volcengine/ConversationalAI-Embedded-Kit-2.0) — 火山实时对话 SDK

# StackChan VolC Open

> 在 [M5Stack StackChan](https://github.com/m5stack/StackChan) 平台上提供**两套开箱即用的 AI 对话后端**，作为开源参考实现：
>
> - `AppAiAgent`：基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 协议，适合自托管 / 私有部署
> - `AppVolcengineAi`：基于 [VolcEngine ConversationalAI Embedded Kit 2.0](https://github.com/volcengine/ConversationalAI-Embedded-Kit-2.0)，适合云端智能体编排 + 视觉理解
>
> 两个 App 共享板级 HAL（`xiaozhi-board-hal`）、Avatar/LED/Servo 表现层与 6 个 Function Call 工具。

## Highlights

- **双后端共存** — 同一固件，launcher 中选择进入哪个 AI 后端；任意时刻仅一个 App 持有音频/摄像头资源。
- **板级 HAL 复用** — 板级代码独立 component，可被 ESP32 任意 AI 项目引用。
- **云端智能体编排** — 端侧只暴露 6 个原子工具（servo / led / reminder / vision），所有"智能"通过更新云端智能体配置完成，无需 OTA。
- **视觉理解** — 火山后端支持被动取帧 + 单 JPEG 上传，按云端 `vision_frame_required` 事件触发。
- **零凭据仓库** — 所有 ProductKey/Secret/AccessKey 走 NVS 或 `sdkconfig.local`，仓库严禁明文。

## Quick Start

```bash
# 1. 克隆项目并拉取上游依赖
git clone https://github.com/<your-org>/stackchan-volc-open.git
cd stackchan-volc-open
./scripts/fetch_repos.sh        # 拉取 xiaozhi-board-hal / xiaozhi-app / volc_conv_ai

# 2. 配置凭据（不进入 git）
cp firmware/sdkconfig.local.example firmware/sdkconfig.local
$EDITOR firmware/sdkconfig.local                     # 填入 BotID / AccessKey 等

# 3. 构建并烧录
cd firmware
idf.py set-target esp32s3
idf.py build flash monitor
```

## Architecture

```
              ┌─────── AppLauncher (互斥高亮) ────────┐
              │                                      │
  AppAiAgent (Xiaozhi)            AppVolcengineAi (火山)
              │                                      │
              └──── AiBackendArbiter (互斥) ─────────┘
                            │
              AiAgentBridge (字幕/情绪/嘴型事件)
                            │
            xiaozhi-board-hal (板级，共享)
                            │
                M5Stack CoreS3 / ESP32-S3
```

详细架构、互斥约束、UI 规范、Function Call schema 见 [docs/DESIGN.md](docs/DESIGN.md)。
落地手册见 [docs/PORTING.md](docs/PORTING.md)。

## Function Call Tools

设备只暴露 6 个原子能力，云端智能体（火山方舟 / Xiaozhi 后端）通过 schema 配置编排：

| Tool                          | 描述                          |
| ----------------------------- | ----------------------------- |
| `self.robot.get_head_angles`  | 读舵机当前 yaw/pitch          |
| `self.robot.set_head_angles`  | 设置舵机目标位置（带速度）    |
| `self.robot.set_led_color`    | 设置左右 NeonLight 颜色       |
| `self.robot.create_reminder`  | 创建定时提醒                  |
| `self.robot.stop_reminder`    | 取消提醒                      |
| `self.robot.capture_vision`   | 单帧 JPEG 上传至云端视觉通道  |

完整 JSON schema 见 [docs/DESIGN.md §5.3](docs/DESIGN.md)。

## Hardware

- M5Stack CoreS3（默认）
- 任何 [xiaozhi-board-hal](https://github.com/<org>/xiaozhi-board-hal) 已支持的 ESP32-S3 板（理论可移植）

## License

Apache-2.0. See [LICENSE](LICENSE).

第三方组件版权另见 [THIRD_PARTY_NOTICE.md](THIRD_PARTY_NOTICE.md)。

## Acknowledgments

- [m5stack/StackChan](https://github.com/m5stack/StackChan) — 原硬件 + 表情/舵机表达框架
- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — Xiaozhi 协议与板级 HAL 基础
- [volcengine/ConversationalAI-Embedded-Kit-2.0](https://github.com/volcengine/ConversationalAI-Embedded-Kit-2.0) — 火山实时对话 SDK

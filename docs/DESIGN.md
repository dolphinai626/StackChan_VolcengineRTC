# Design

> 本项目（stackchan-volc-open）的完整架构设计。简化版 PORTING 见 [PORTING.md](PORTING.md)。

## 目标

在 [M5Stack StackChan](https://github.com/m5stack/StackChan) 平台上提供两套**开箱即用**的 AI 对话后端，作为开源参考实现：
- `AppAiAgent`：基于 78/xiaozhi-esp32（自托管派）
- `AppVolcengineAi`：基于 VolcEngine ConversationalAI Embedded Kit 2.0（云服务派 + 视觉理解）

两个 App 共享板级 HAL、Avatar/LED/Servo 表现层、6 个 Function Call 工具。
**任意时刻仅一个 AI App 持有音频/摄像头资源**。

---

## 一、总体架构

```
                AppLauncher (互斥高亮)
                       │
        ┌──────────────┼──────────────┐
        │                             │
   AppAiAgent                  AppVolcengineAi
   (Xiaozhi)                   (Volcengine)
        │                             │
        └──── AiBackendArbiter ───────┘
                       │
        AiAgentBridge (字幕/情绪/嘴型事件)
                       │
   xiaozhi-board-hal (板级 HAL，共享)
                       │
            M5Stack CoreS3 / ESP32-S3
```

三层定位：
- **L1 板级层**：[xiaozhi-board-hal](https://github.com/<org>/xiaozhi-board-hal)，物理资源所有者
- **L2 后端层**：`xiaozhi-app` + `volc_conv_ai` 两个独立 component
- **L3 业务层**：两个 Mooncake App + 共享 `AiAgentBridge`

---

## 二、设计原则

1. **互斥独占**：`AiBackendArbiter` 强制
2. **板级共享**：硬件访问只走 `hal_bridge::*`，禁止直连 `i2s_*` / `esp_camera_*`
3. **后端隔离**：xiaozhi 与 volc 业务互不 include
4. **表现层共享**：Avatar/LED/Servo 渲染管线只一份
5. **及时释放**：`onClose` 完成 task join + socket close + PSRAM 释放
6. **云端编排**：FunctionCall/MCP 在云端配置；端侧只暴露原子能力

---

## 三、互斥与生命周期

### 3.1 AiBackendArbiter

```cpp
namespace ai_backend {
  enum class Owner { None, Xiaozhi, Volcengine };
  bool acquire(Owner);   // 非阻塞
  void release(Owner);   // 仅 owner 调用生效
  Owner current();
}
```

- `std::mutex` + `std::atomic<Owner>`
- `release` 后强制 `vTaskDelay(50)` 让 audio task 优雅结束

### 3.2 onOpen 模板

```
1. ai_backend::acquire(Owner::Xxx)        // 失败 → 退出
2. wifi_connect(15s)
3. audio_acquire("owner_tag")
4. 启动后端引擎 + tools 注册
5. 启动摄像头 passive frame task（仅 Volc）
6. 订阅 AiAgentBridge
7. 切换 UI 到对话视图
```

### 3.3 onClose 模板（强制对称）

```
1. _shutting_down = true
2. 反向回滚已完成阶段，幂等可重入
3. 后端引擎 stop + task join (timeout 500ms)
4. 注销 bridge / tools / audio / acquire
```

### 3.4 Launcher UI 互斥

- 当前 owner 卡片标 "● 运行中"，主题色保持
- 非 owner 卡片置灰，标 "退出 [owner] 后启用"
- 点击非 owner 卡片只 toast，不弹起 App

---

## 四、UI 风格对齐

| 组件                 | 行为                                                     |
| -------------------- | -------------------------------------------------------- |
| `AiAgentMainView`    | 全屏 Avatar + 顶部状态条 + 底部字幕条（共用） |
| `SubtitleStrip`      | 流式字幕 2 行                                |
| `EmotionDecorator`   | 复用 angry/dizzy/heart/shy/sweat 表情贴纸 |

主题色：Xiaozhi `#33CC99`，Volcengine `#3A8DFF`。

---

## 五、Function Call 工具

### 5.1 端侧

`main/stackchan/robot_tools/`：
- `robot_tools.{h,cpp}` 后端无关
- `adapter_xiaozhi.cpp` 注册到 mcp_server
- `adapter_volcengine.cpp` 注册到 function_call_service dispatch

6 个工具：
| Tool                         | 描述                       |
| ---------------------------- | -------------------------- |
| `self.robot.get_head_angles` | 读舵机当前 yaw/pitch       |
| `self.robot.set_head_angles` | 设置舵机角度（带速度）     |
| `self.robot.set_led_color`   | 设置左右 NeonLight 颜色    |
| `self.robot.create_reminder` | 创建提醒                   |
| `self.robot.stop_reminder`   | 取消提醒                   |
| `self.robot.capture_vision`  | 单帧 JPEG 上传             |

### 5.3 服务端 Schema（火山方舟 / Xiaozhi 后端通用）

#### `self.robot.get_head_angles`
```json
{
  "name": "self.robot.get_head_angles",
  "description": "Get StackChan's current head yaw and pitch in degrees.",
  "parameters": { "type": "object", "properties": {}, "required": [] }
}
```

#### `self.robot.set_head_angles`
```json
{
  "name": "self.robot.set_head_angles",
  "description": "Move StackChan's head. Stay within +/-45 deg for natural conversation.",
  "parameters": {
    "type": "object",
    "properties": {
      "yaw":   { "type": "integer", "minimum": -128, "maximum": 128 },
      "pitch": { "type": "integer", "minimum": 0,    "maximum": 90  },
      "speed": { "type": "integer", "minimum": 100,  "maximum": 1000, "default": 150 }
    },
    "required": ["yaw", "pitch"]
  }
}
```

#### `self.robot.set_led_color`
```json
{
  "name": "self.robot.set_led_color",
  "description": "Set onboard NeonLight color for emotional expression. Off=0,0,0.",
  "parameters": {
    "type": "object",
    "properties": {
      "red":   { "type": "integer", "minimum": 0, "maximum": 168 },
      "green": { "type": "integer", "minimum": 0, "maximum": 168 },
      "blue":  { "type": "integer", "minimum": 0, "maximum": 168 }
    },
    "required": ["red", "green", "blue"]
  }
}
```

#### `self.robot.create_reminder`
```json
{
  "name": "self.robot.create_reminder",
  "description": "Create a timed reminder. Returns reminder id.",
  "parameters": {
    "type": "object",
    "properties": {
      "duration_seconds": { "type": "integer", "minimum": 1, "maximum": 86400 },
      "message":          { "type": "string",  "maxLength": 128 },
      "repeat":           { "type": "boolean", "default": false }
    },
    "required": ["duration_seconds", "message"]
  }
}
```

#### `self.robot.stop_reminder`
```json
{
  "name": "self.robot.stop_reminder",
  "description": "Stop an active reminder by id.",
  "parameters": {
    "type": "object",
    "properties": { "id": { "type": "integer" } },
    "required": ["id"]
  }
}
```

#### `self.robot.capture_vision`
```json
{
  "name": "self.robot.capture_vision",
  "description": "Capture a single JPEG frame for cloud vision understanding.",
  "parameters": {
    "type": "object",
    "properties": {
      "quality": { "type": "integer", "minimum": 10, "maximum": 80, "default": 20 }
    }
  }
}
```

### 5.4 服务端 system prompt 模板

```
You are an embodied robot named StackChan. You can:
- Move your head with set_head_angles (read current pose first via get_head_angles).
- Express emotion through set_led_color: green=happy, red=alert, blue=thinking, off=idle.
- Set reminders with create_reminder; cancel them with stop_reminder.
- Look at the user's environment with capture_vision when they ask about what you can see.
Respond naturally and use tools sparingly to enhance interaction, not replace conversation.
```

---

## 六、目录结构

```
stackchan-volc-open/
├── README.md / LICENSE / THIRD_PARTY_NOTICE.md
├── repos.json                       # 上游依赖清单
├── docs/
│   ├── DESIGN.md                    # 本文件
│   └── PORTING.md
├── scripts/
│   ├── fetch_repos.sh               # 拉 upstream
│   └── scan_secrets.py              # 凭据正则扫描
├── upstream/                        # fetch_repos.sh 写入；不进 git
│   ├── xiaozhi-board-hal/
│   ├── xiaozhi-app/
│   └── volc_conv_ai/
└── firmware/
    ├── CMakeLists.txt
    ├── partitions.csv               # 6MB app, OTA ping-pong
    ├── sdkconfig.defaults
    ├── sdkconfig.local.example
    └── main/
        ├── CMakeLists.txt
        ├── Kconfig.projbuild
        ├── main.cpp
        ├── hal/
        │   ├── hal.{h,cpp}          # 系统级 boot
        │   └── hal_bridge.{h,cpp}   # 后端无关硬件接口
        ├── stackchan/
        │   ├── ai_backend_arbiter.{h,cpp}
        │   ├── ai_agent_bridge.{h,cpp}
        │   └── robot_tools/
        │       ├── robot_tools.{h,cpp}
        │       ├── adapter_xiaozhi.cpp
        │       └── adapter_volcengine.cpp
        └── apps/
            ├── app_base.h
            ├── app_launcher/
            ├── app_ai_agent/
            ├── app_volcengine_ai/
            ├── app_avatar/
            └── app_setup/
```

---

## 七、凭据管理

> 公开仓库严禁明文凭据。所有真实凭据走 NVS 或 `sdkconfig.local`（已 `.gitignore`）。

### 7.1 Kconfig 占位

`firmware/main/Kconfig.projbuild` 提供 5 个空占位：
`VOLC_BOT_ID` / `VOLC_INSTANCE_ID` / `VOLC_PRODUCT_KEY` / `VOLC_PRODUCT_SECRET` / `VOLC_DEVICE_NAME`。

### 7.2 NVS 兜底

运行时优先读 NVS namespace `volc_agent`（由 `app_setup` 写入），缺失时回落 `CONFIG_VOLC_*`。

### 7.3 本地开发

`cp firmware/sdkconfig.local.example firmware/sdkconfig.local` 后填值，免烧 NVS。

### 7.4 secret 扫描

`scripts/scan_secrets.py` 检测：
- `[a-f0-9]{24}` ProductSecret
- `bot[A-Za-z0-9]{8,16}` BotID
- `AKLT[...]` AccessKey
- `[a-f0-9]{32}` RTC AppKey

每次 `fetch_repos.sh` 后自动执行；CI 中作为 pre-merge gate。

---

## 八、性能与资源

| 维度        | 现状      | 双 App 后  | 处置                |
| ----------- | --------- | ---------- | ------------------- |
| 运行期 CPU  | 无变化    | 无变化     | 互斥执行            |
| 运行期 RAM  | 无变化    | 无变化     | 互斥执行            |
| Flash       | -         | ~3.6 MB    | partitions.csv 6MB  |
| BSS         | -         | +30~80 KB  | PSRAM 吸收          |
| 切换延时    | -         | 100~500 ms | UI toast 提示       |

---

## 九、实施路线

- **M0 骨架**（当前）：目录、CMake、AiBackendArbiter、AiAgentBridge、RobotTools 双 adapter、HAL stub、App 空壳
- **M1 上游接入**：fetch_repos.sh 跑通；xiaozhi-board-hal / xiaozhi-app / volc_conv_ai 真实编译
- **M2 双 App 互斥跑通**：launcher 互斥高亮；两后端能各自跑完整对话
- **M3 视觉与编排**：被动取帧；火山方舟智能体 6 工具配置；Xiaozhi MCP 端侧注册
- **M4 OTA + 配网**：esp_https_ota；app_setup workers 双后端凭据 SoftAP 配网
- **M5 polish**：README 双 App 故事；PORTING 终稿；CI

---

## 十、License

Apache-2.0；第三方 component 版权见 [THIRD_PARTY_NOTICE.md](../THIRD_PARTY_NOTICE.md)。

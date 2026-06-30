# 云端智能体 Tools 配置手册（volcRTC 链路）

> 本文档与端侧实现严格对应（`firmware/main/apps/app_ai_agent/volc_agent.cpp` 的
> `dispatchTool`）。修改端侧工具时必须同步更新本文档。
> xiaozhi 链路同等能力经 MCP 自动注册（`firmware/main/hal/hal_mcp.cpp`），无需云端配置。

## 1. 投递方式（必配，否则 LLM 调工具后永远无回复）

智能体的 **FunctionCallingConfig 必须配置为客户端投递（RTS 房间消息）**。
为空时后端报 `http sender is nil`，工具调用被静默丢弃，LLM 等不到结果本轮挂起
（已踩坑，详见 debug-llm-no-reply-fc-config.md）。

端侧 RTS 协议（与官方 function_call_service 一致，已实现并对齐）：

| 方向 | magic | payload |
| --- | --- | --- |
| 服务端→设备（触发通知，可选） | `info` | `{"event_type":"function_calling","function":"<name>","tool_call_id":"..."}` |
| 服务端→设备（工具调用） | `tool` | `{"tool_calls":[{"id":"call_xxx","type":"function","function":{"name":"<name>","arguments":"<json字符串>"}}]}` |
| 设备→服务端（结果回传） | `func` | `{"ToolCallID":"call_xxx","Content":"<json字符串>"}` |

二进制消息格式：4 字节 magic + 4 字节大端 payload 长度 + JSON。

## 2. 工具 Schema（共 9 个，直接粘贴到智能体 Tools 配置）

### self.robot.get_head_angles
```json
{
  "name": "self.robot.get_head_angles",
  "description": "Get StackChan's current head yaw and pitch in degrees. Neutral is {yaw:0,pitch:0}.",
  "parameters": { "type": "object", "properties": {}, "required": [] }
}
```
返回：`{"yaw":<int>,"pitch":<int>}`

### self.robot.set_head_angles
```json
{
  "name": "self.robot.set_head_angles",
  "description": "Move StackChan's head. Stay within +/-45 deg for natural conversation; only exceed 70 if user explicitly asks to look far away. Yaw -128(left)~128(right), pitch 0~90(up).",
  "parameters": {
    "type": "object",
    "properties": {
      "yaw":   { "type": "integer", "minimum": -128, "maximum": 128 },
      "pitch": { "type": "integer", "minimum": 0,    "maximum": 90  },
      "speed": { "type": "integer", "minimum": 100,  "maximum": 1000, "default": 150 }
    },
    "required": []
  }
}
```
返回：`{"ok":true}`

### self.robot.shake_head
```json
{
  "name": "self.robot.shake_head",
  "description": "Shake head left-right to express denial or playfulness.",
  "parameters": {
    "type": "object",
    "properties": {
      "times":     { "type": "integer", "minimum": 1,   "maximum": 5,    "default": 2 },
      "amplitude": { "type": "integer", "minimum": 5,   "maximum": 60,   "default": 25 },
      "speed":     { "type": "integer", "minimum": 100, "maximum": 1000, "default": 250 },
      "pause_ms":  { "type": "integer", "minimum": 50,  "maximum": 500,  "default": 180 }
    },
    "required": []
  }
}
```
返回：`{"ok":true}`

### self.robot.set_led_color
```json
{
  "name": "self.robot.set_led_color",
  "description": "Set onboard LED color for emotional expression. Range 0-168 per channel. Red=168,0,0; Green=0,168,0; Blue=0,0,168; Off=0,0,0.",
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
返回：`{"ok":true}`

### self.robot.create_reminder
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
    "required": ["duration_seconds"]
  }
}
```
返回：`{"id":<int>}`

### self.robot.get_reminders
```json
{
  "name": "self.robot.get_reminders",
  "description": "List all active (not-yet-fired) reminders.",
  "parameters": { "type": "object", "properties": {}, "required": [] }
}
```
返回：`[{"id":..,"duration_ms":..,"message":"..","repeat":..}]`

### self.robot.stop_reminder
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
返回：`{"ok":true}`

### self.robot.get_volume
```json
{
  "name": "self.robot.get_volume",
  "description": "Get current speaker volume (0-100).",
  "parameters": { "type": "object", "properties": {}, "required": [] }
}
```
返回：`{"volume":<int>}`

### self.robot.set_volume
```json
{
  "name": "self.robot.set_volume",
  "description": "Set speaker volume 0-100. Call when user asks louder/quieter. 0=mute.",
  "parameters": {
    "type": "object",
    "properties": {
      "volume": { "type": "integer", "minimum": 0, "maximum": 100, "default": 70 }
    },
    "required": ["volume"]
  }
}
```
返回：`{"ok":true,"volume":<int>}`

## 3. 内置云端工具（控制台智能体勾选启用，无需端侧实现）

除上面 9 个端侧工具外，火山智能体还在云端启用了两个**内置工具**。它们在控制台
智能体 Config 里开关，由火山侧执行，端侧无需实现，也不走 RTS `tool`/`func` 回包，
因此屏幕不会显示 `[工具] xxx` 调用字幕。

### WebSearch（联网搜索）

`WebSearchAgentConfig.Enable = true`，用于查询时效性信息。

| 字段 | 值 |
| --- | --- |
| FunctionName | `WebSearch` |
| FunctionDescription | 查询实时信息，如今天的天气、最新的新闻、A 股票的当前价格等 |
| ComfortWords | 让我打开雷达搜索一下。 |
| ParamsString | `{"bot_id":"<联网搜索 bot id>","stream":true}` |
| APIKey | 由 `WEBSEARCH_API_KEY` 注入（不进 git） |

System Prompt 约束：WebSearch 只用于天气、新闻、股价、日期、最新政策等时效性信息；
模型能直接回答时不要调用，避免无谓搜索延迟。

### 音乐播放（MusicAgent）

`MusicAgentConfig.Enable = true`，触发函数 `music_player`，用户意图为播放/控制音乐时触发。

System Prompt 约束：仅当用户明确要播放或控制音乐时才触发 `music_player`；用户只说
"暂停 / 停止 / 停一下"等模糊指令且上一轮与音乐无关时，视为停止对话直接文字回应，
不得调用 `music_player`。

## 4. 在控制台智能体自定义新增工具

火山控制台智能体支持在 Tools / Config 里**自定义新增工具**，分两类：

- **端侧执行的工具**：按第 2 节 JSON schema 格式在智能体 Tools 里新增 function，并保证
  `FunctionCallingConfig` 为客户端投递（RTS）。设备侧必须在 `volc_agent.cpp` 的
  `dispatchTool` 里实现同名分支并回 `func` 包，否则 LLM 调用后会挂起等不到结果。
  **新增端侧工具时，端侧 `dispatchTool`、本文档第 2 节、README 三处必须同步更新。**
- **云端执行的内置 / 托管工具**（如 WebSearch、MusicAgent 或其他火山提供的 Agent 能力）：
  在控制台对应 Config 开关启用即可，端侧无需改动。

> 命名建议：端侧工具沿用 `self.robot.*`，云端内置工具沿用火山函数名，避免与现有工具
> 重名导致分发歧义。

## 5. System Prompt 模板（建议）

```
你是名为 StackChan 的桌面机器人，回复会被语音合成朗读并显示为字幕：
只输出纯口语化文本，禁止 Markdown 符号（#、*、列表），单次回复不超过 3 句话，
不要说"让我搜索一下"等过程性语句。

你可以使用这些身体能力，工具调用要克制、为对话加分而不是代替对话：
- set_head_angles 转头看向用户（先用 get_head_angles 读当前位置）
- shake_head 摇头表达否定或俏皮
- set_led_color 用灯色表达情绪：绿=开心，红=警示，蓝=思考，全 0=熄灭
- create_reminder/get_reminders/stop_reminder 管理定时提醒
- set_volume/get_volume 按用户要求调音量
```

## 6. 两链路能力对照

| 能力 | volcRTC（云端 Tools 配置） | xiaozhi（MCP 自动注册） |
| --- | --- | --- |
| get/set_head_angles | self.robot.* | self.robot.*（同名） |
| shake_head | self.robot.shake_head | self.robot.shake_head（同名） |
| set_led_color | self.robot.set_led_color | self.robot.set_led_color（同名） |
| reminder 三件套 | self.robot.* | self.robot.*（同名） |
| 音量 | self.robot.get/set_volume | xiaozhi 内建 `self.get_device_status` / `self.audio_speaker.set_volume` |
| 联网搜索 | WebSearch（云端内置） | —（xiaozhi 链路无） |
| 音乐播放 | music_player（云端内置 MusicAgent） | —（xiaozhi 链路无） |
| 调用过程字幕 | `[工具] <名> 调用中/执行中/完成`（仅端侧工具） | 同左 |
| 唤醒前声源定位 | AFE DOA，唤醒瞬间转头朝向声源 | —（xiaozhi 链路无 DOA） |

## 7. 配置后验证清单

1. 串口应出现：`function_calling triggered: ...`（info 消息，若服务端下发）
   → `tool call: self.robot.xxx args=...` → `tool result sent ret=0`
2. 屏幕字幕依次显示：`[工具] xxx 调用中` → `[工具] xxx 执行中` → `[工具] xxx 完成`
3. 说"把灯变成红色"应看到 LED 变红且 LLM 有口头确认（不再挂起超时）
4. `tool result sent ret=` 非 0 说明回包发送失败，查 RTS 通道

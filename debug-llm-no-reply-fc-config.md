# Debug Session: llm-no-reply (function calling 投递配置缺失)

Status: [ROOT CAUSE CONFIRMED - 云端配置问题，待控制台整改]

## Symptoms

- RTP 时钟修复后 ASR 字幕完整准确，但部分轮次 LLM 无回复。
- 设备侧表现：`conv_status=2`(thinking) 后无下行，15s idle timeout 回待机。
- 偶发 `subtitle[bot] red。` 之类的单词级垃圾回复。

## Evidence

- 服务端 stdout2.txt：每次 LLM 触发 function_calling 后立刻报错：
  - `llm_client.go:694 notify triggered event, Type:function_calling, functionCallingCtx:&{self.robot.set_led_color call_xxx ...}`
  - `voice_chat.go:2623 [VoiceChatLauncher] http sender is nil.` ← 工具调用投递失败
- 服务端 agent 启动配置（call2ServerData）：`"FunctionCallingConfig":{}` 为空。
- 本次会话 LLM 共发起 17 次工具调用（set_led_color×9、get/set_head_angles×8），设备串口 0 条 `tool call:` 日志——工具调用从未到达设备。
- `[BiDirectionTTS] tts sentence end, but has no words, text:":.` 与设备收到的
  `subtitle[bot] red。` 互证：tool_call 流式片段泄漏进 TTS 文本通道。
- 无工具调用的轮次（如"讲小白兔的故事"）LLM 回复正常 → LLM/TTS 链路本身健康。

## Root Cause

智能体配置了 9 个端侧工具 schema，但 **FunctionCallingConfig（投递方式）为空**：
LLM 发起 function call 后，voicechat 后端既没有走 RTS 房间消息投递给客户端
（设备实现的 `tool` magic 通道），也没有 HTTP 端点可投递（http sender nil），
调用被丢弃，LLM 永远等不到 tool result，本轮回复挂起。

## Fix (云端，控制台/StartVoiceChat 配置)

1. 在智能体配置中把 Function Call 投递方式配置为**客户端投递**（RTS 消息到房间，
   即 `tool` 二进制消息 + 客户端 `func` 回包），与端侧实现及 volc_conv_ai SDK
   官方协议一致。
2. 或临时验证：先把 9 个工具 schema 从智能体移除 → LLM 回复应立即恢复正常，
   可 100% 隔离确认本结论。
3. 顺带检查 WebSearchAgentConfig（已启用，bot_id 7508945078770058793）：此前
   thinking 挂死（"让我打开雷达搜索一下"后无响应）疑似该插件超时，无错误事件。

## 端侧待办（云端修复后回归）

- 设备 `tool` magic → `volc_tool` 任务 → `func` 回包链路从未被真实流量验证过，
  云端投递打通后需第一时间回归 9 个工具。

## 关联结论（同日已解决）

- 上行截断根因：RTC opus 参数 `s_samples_per_frame` 被误改为 320（应为 RFC 7587
  的 48k 时钟 960），RTP 时间戳走速 1/3，服务端 jitter buffer 挤压丢弃人声。
  恢复 48000/960 后 ASR 完整准确。详见 debug-uplink-stream-drop.md。

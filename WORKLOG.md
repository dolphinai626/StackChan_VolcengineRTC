# WORKLOG — 会话工作日志（新条目加在最上面）

> 每个 AI 会话开工时在此登记，收工时更新结果。格式：
> `## YYYY-MM-DD [工具] 任务一句话` + 占用域 + 结果/遗留。

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

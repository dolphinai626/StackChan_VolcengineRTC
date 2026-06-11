# Debug Session: uplink-stream-drop

Status: [RESOLVED - 根因为 RTC opus RTP 参数错误，详见文末 Resolution]

## Symptoms

- 上行语音说长句时采集提前终止，ASR 只保留开头。
- 提前进入思考状态后可能卡死。
- 需要保障麦克风采集持续存在，并确认当前采集增益是否过低。

## Hypotheses

1. 会话状态机把 `listening` 过早切到 `thinking/answering`，导致上行发送门控提前关闭。
2. 半双工回声门控或播放尾音窗口误判，丢弃了用户仍在说话的麦克风帧。
3. 上行音频队列满或网络发送阻塞，导致 AFE fetch 持续采集但上传侧丢帧/停止。
4. VAD/服务端 `conv_status` 事件被误解释为用户说完，客户端提前停止上行。
5. ES7210 输入增益偏低或 AFE 能量阈值不足，长句后半段被判静音。

## Evidence Plan

- 先读取当前上行采集、状态切换、字幕、音频增益相关代码。
- 复用已有串口日志，定位 `listening/thinking/answering`、uplink 队列、AFE peak/clip、subtitle 序列变化。
- 若现有日志不足，只添加诊断日志，不改业务逻辑。

## Evidence

- `/tmp/sc_log6.txt` 1518-1733：服务端在用户字幕仅到“讲一个。”时发送 `conv_status=2`，随后进入 `conv_status=3`。
- `/tmp/sc_log6.txt` 1712：同一时刻本地 AFE 仍有上行帧 `rms=1365 peak=3162 clip=0`，说明硬件采集没有停止。
- `/tmp/sc_log6.txt` 1758：下行音频在 `ANSWERING` 之后约 640ms 才到达，旧逻辑在这段无播放窗口也把上行替换成静音。
- `/tmp/sc_log6.txt` 1：`minimal sram=8579`，播放队列 256 包会放大长回复时的内部 SRAM 压力。

## Fix

- `ANSWERING` 状态本身不再触发上行静音门控，仅在本地播放队列排空中或尾音窗口内门控。
- 播放队列上限从 256 降到 96，避免长回复把内部 SRAM 压到危险水位。

## Post-fix Evidence

- `/tmp/sc_uplink_fix_log.txt` 335-339：用户轮次 `uplink env avg=627 max=1390 gated=0/50`，随后服务端只定稿“我今天不。”并进入 `conv_status=2`。
- `/tmp/sc_uplink_fix_log.txt` 390-412：`conv_status=2` 后连续 15 秒仍有 `uplink env ... gated=0/50`，说明本地流式上传没有中断；最终是服务端 thinking 无响应后本地 idle timeout。
- `/tmp/sc_uplink_fix_log.txt` 336、366、391：ASR 文本分别为“我今天不。”、“你给我。”、“你到底在干嘛？”，前两句明显短截/误识别。
- 用户语音上行能量偏低（avg 600-1000 量级），ES7210 硬件增益已为 36dB，继续加硬件 PGA 有削波风险；下一步改为上行软件增益 + 限幅。

## Resolution (2026-06-11)

- 根因：RTC 配置 `params` 中 opus `s_samples_per_frame` 被误设为 320 /
  `sample_rate` 16000。Opus 的 RTP 时间戳时钟按 RFC 7587 恒为 48000，20ms 包
  应步进 960；按 320 打时间戳后走速只有正确值的 1/3，服务端 jitter buffer 按
  48k 时钟收流时大量挤压丢弃——服务端 dump 表现为"端侧停发/人声截断"，ASR
  乱码且过早断句，而端侧全链路日志正常（drop=0 peak=1）。该值是早期排查播放
  栈溢出时从 960 误改的（那次只需改本地解码器帧长）。
- 修复：恢复 `"opus":{"sample_rate":48000,"channels":1,"s_samples_per_frame":960}`，
  端侧编码保持 16k PCM 输入不变。
- 验证：修复后长句 ASR 完整准确（"我今天不太开心，你给我讲个笑话。"等全句定稿），
  服务端收流正常。
- 同轮顺带修复的真实问题（被 RTP 根因掩盖）：uplinkTask 每帧 vTaskDelay(1)=10ms
  吞吐天花板（改 taskYIELD）；Volc 会话未关 WiFi 省电（MIN_MODEM 周期性 TX 停顿，
  现会话期 WIFI_PS_NONE）；`_mutex` 跨网络发送导致 UI 卡死（拆分 _send_mutex）。
- 后续新问题（LLM 无回复）→ debug-llm-no-reply-fc-config.md。

## Code Review Check (2026-06-11)

- 检查范围：`volc_agent.cpp`、`cores3_audio_codec.cc`、`stackchan_display.cc` 及本 debug 记录。
- 关键确认：RTC Opus RTP 参数已恢复为 48k/960；上行编码仍使用 16k PCM；上行任务不再每帧 `vTaskDelay(1)`；会话期关闭 WiFi 省电；发送路径使用 `_send_mutex -> _mutex` 锁序避免销毁竞态。
- 上行门控仍只在 `ANSWERING` 丢帧，不发送静音帧；`uplink16 env` 日志保留 `avg/max/clip/drop/peak/rs_out`，可继续确认增益、削波与队列丢帧。
- 未发现需要阻断提交的 P0-P2 级代码缺陷；待构建验证通过后提交。

# Debug Session: wakeup-source-localization

Status: [OPEN - wake paths verified, physical source-direction check pending]

## Symptoms
- StackChan/xiaozhi 链路无法唤醒对话。
- Volcengine 链路无法唤醒对话。
- 声源定位功能异常。

## Hypotheses
1. AFE/WakeNet 任务没有正常启动，或任务栈/内存分配失败。
2. 音频输入通道、采样率或 AFE feed 数据格式配置不匹配，导致 WakeNet 收不到有效音频。
3. WakeNet 已检测到唤醒词，但链路状态机没有从唤醒事件进入会话启动。
4. 声源定位依赖的 `trigger_channel_id` 与 CoreS3 麦克风/参考通道映射不一致。
5. 最近 MIC3/AEC/reference 通道调整影响了 WakeNet/声源定位的输入通道。

## Evidence Plan
- 先读取 StackChan/xiaozhi 与 Volcengine 两条链路的 AFE 初始化、feed/fetch、唤醒状态机和声源定位逻辑。
- 抓取设备串口日志，确认是否有 AFE 初始化失败、任务创建失败、WakeNet 检测事件或异常重启。
- 如静态阅读和现有日志不足，再只添加观测日志，不做业务逻辑修复。

## Findings
- Static finding: main CoreS3 codec reports 3 input channels when reference is enabled, while the upstream xiaozhi CoreS3 codec reports 2 input channels.
- Static finding: main CoreS3 codec still initializes RX with TDM slot mask 0-3 and stereo slot mode; need runtime confirmation that esp_codec_dev actually returns valid channel 0/1/2 audio.
- Instrumentation added to `firmware/main/hal/board/cores3_audio_codec.cc` to log input open parameters and first read RMS per channel.
- Runtime evidence: device logs show `I2S_IF: Not support channel 3`, then `esp_codec_dev_read` returns `ESP_FAIL`, all probed RMS values are `[0,0,0]`, and AFE repeatedly logs `Ringbuffer of AFE is empty`.
- Conclusion: current 3-channel codec input path is unsupported, so both StackChan/xiaozhi and Volc WakeNet receive no audio. Source localization also cannot work because no valid mic frames reach AFE/DOA.

## Fix
- Change CoreS3 audio input back to supported 2-channel microphone input for wake/source localization.
- Do not attempt MIC3 reference through `esp_codec_dev` 3-channel mode; this requires a separate raw TDM path if needed later.
- Applied: `AUDIO_INPUT_REFERENCE=false`, `CoreS3AudioCodec` now opens ES7210 MIC1/MIC2 as two input channels and sets gain for both channels.

## Verification
- `idf.py -p /dev/cu.usbmodem2101 build flash` passed.
- Volcengine path verified with valid AFE feed/uplink frames, conversation status transitions, and downstream audio:
  - `afe feed frames=400`
  - `afe uplink frames=50`
  - `conv_status=1`
  - `conv_status=3`
  - `audio_dl bytes=160`
- StackChan/xiaozhi path verified with wake word detection and normal dialog state transitions:
  - `Wake word detected: Hi,Stack Chan`
  - `State: idle -> connecting`
  - `State: connecting -> listening`
- StackChan tool call path was also observed working through `self.robot.set_head_angles`.
- Remaining check: physical source-direction turn accuracy still needs real-world confirmation after the two-mic input recovery.
- Observation: StackChan dialog can still emit `AFE(FEED) is full` warnings; this did not block wake/dialog in the verified run.

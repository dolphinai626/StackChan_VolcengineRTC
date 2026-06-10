Status: [RESOLVED]

# Debug Session: volc-wake-reboot

## Symptoms
- Volc 链路唤醒后设备直接重启。
- 字幕气泡当前偏大，需要恢复默认大小。

## Hypotheses
1. Opus 20ms 编码输入尺寸与 encoder 期望不一致，唤醒后首次上行触发异常。
2. 唤醒后 `volc_start` 与 AFE fetch/audio 上行并发导致状态或指针异常。
3. Opus encoder/decoder 初始化后的内存余量不足，唤醒后 RTC 建联或任务运行触发堆/栈问题。
4. 服务端下行帧长与本地 60ms decoder 配置不匹配，收到首个下行音频时解码异常。
5. 声源定位转头在唤醒路径中触发舵机/LVGL 并发异常。

## Evidence
- Pre-fix serial log captured after entering Volc mode and wake word:
  - `wake word detected, starting conversation`
  - `wake source direction=100 yaw=10`
  - `external date prompt sent: 今天是2026年06月05日`
  - `afe uplink frames=1 vad=0 samples=320 out_rms=132`
  - `***ERROR*** A stack overflow in task volc_play has been detected.`
  - Device rebooted with `rst:0xc (RTC_SW_CPU_RST)`.
- Hypothesis status:
  1. Opus encoder input mismatch: rejected. Encoder reported `input_bytes=640`, first uplink frame used `samples=320`, matching 640 bytes.
  2. `volc_start`/AFE pointer race: not supported by evidence. Crash names `volc_play`, not AFE fetch or SDK start task.
  3. Memory/stack issue: confirmed specifically as `volc_play` task stack overflow.
  4. Downlink decoder frame mismatch / frame sizing: still plausible. After increasing `volc_play` stack to 8192 bytes, reboot still reproduced with the same `volc_play` stack overflow.
  5. Wake-source turn path: rejected. Direction log completed before crash; crash task was `volc_play`.
- Latest 4-minute capture reproduced the reboot multiple times:
  - `wake word detected, starting conversation`
  - `wake source direction=100 yaw=10`
  - `external date prompt sent: 今天是2026年06月05日`
  - `afe uplink frames=1 vad=0 samples=320 out_rms=29/46/66`
  - `***ERROR*** A stack overflow in task volc_play has been detected.`
  - Backtrace was corrupted and reboot followed with `rst:0xc (RTC_SW_CPU_RST)`.
- Same capture also showed a separate cloud/API failure path:
  - `GetRTCConfig` returned HTTP 500 / `InternalError`.
  - After that, repeated wake attempts failed with `engine is not in CREATED state`.
  - This explains a stuck/non-starting session after a failed start, but it is not the reboot cause.
- First fix attempt moved `volc_play` creation from Volc app start to first downlink audio frame. Result:
  - Previous pre-downlink `volc_play` overflow disappeared.
  - Session reached `connected`, `answering`, and first `audio_dl`.
  - Crash moved to `***ERROR*** A stack overflow in task VolcRTCMain has been detected.`
  - This confirmed Opus decode/resample inside `onAudioData` was overflowing the SDK receive task stack.
- Final fix moved downlink Opus decode/resample/playback out of `onAudioData`:
  - SDK callback now only copies the received Opus packet into `_playback_queue`.
  - Dedicated `volc_play` task pops Opus packets, decodes, resamples, and calls `OutputData`.
  - `volc_play` PSRAM stack increased to 32768 bytes.

## Changes
- Increased `volc_play` task stack from 4096 to 8192 bytes, still allocated in PSRAM.
- Restored default speech bubble geometry and scrolling behavior.
- After user confirmation that reboot still reproduces, changed downlink Opus frame duration from 60ms to 20ms and RTC `s_samples_per_frame` from 960 to 320.
- Replaced `volc_play` C++ `condition_variable::wait_for` with a FreeRTOS `vTaskDelay` polling loop to reduce playback task stack usage.
- Lazily create `volc_play` on first downlink audio frame instead of at Volc app start.
- Moved downlink Opus decode/resample/playback from SDK callback (`VolcRTCMain`) into `volc_play`.
- Increased `volc_play` PSRAM stack to 32768 bytes.
- Wait for `volc_play` to exit before releasing the Opus decoder during `volc_agent::stop()`.

## Verification
- Build succeeded after changing downlink Opus to 20ms and replacing `volc_play` condition-variable wait with FreeRTOS polling.
- Flash retry completed successfully on `/dev/cu.usbmodem2101`; all written images were hash verified and the device hard reset.
- Serial capture after flashing showed stable idle logs for ~180s:
  - `SystemInfo: free sram: 118667 minimal sram: 117199`
  - No spontaneous reboot observed while idle.
- Final build succeeded.
- Final flash succeeded on `/dev/cu.usbmodem2101`; all written images were hash verified.
- Final 4-minute serial capture after flashing:
  - Volc entered wake-wait mode.
  - Wake word detected.
  - Session reached `connected`, `answering`, `listening`, and repeated `answer finished`.
  - Multiple downlink audio frames were received and played, e.g. `audio_dl bytes=134/128/117/80/122/124/120/137... type=1`.
  - Multiple subtitle messages were received.
  - No `stack overflow` was observed.
  - No reboot was observed.
- Status resolved for the wake-after-reboot issue.

---

# Debug Session: volc-audio-stutter (playback stutter + incomplete capture)

Status: [RESOLVED for stutter/capture; SEPARATE re-entry crash noted below]

## Symptoms
- 播放卡顿严重。
- 音频采集信息不全（`AFE(FEED) Ringbuffer full` 反复出现，疑似丢麦克风帧）。
- 用户要求核对帧率/采样率对齐与时钟稳定性。

## Root Causes (confirmed by code + serial evidence)
1. 采集不全：`afeFetchTask` 在 fetch 循环内内联做 Opus 编码 + `volc_send_audio_data`。网络发送阻塞时 `fetch()` 停止消费，AFE FEED ringbuffer 溢出丢麦克风数据。
2. 播放卡顿/丢音：`playbackTask` 每个下行包只解一次，忽略 `esp_audio_dec_in_raw_t.consumed`。服务端 ~120ms 分包内含多个 20ms Opus 帧，只播了第一帧。
3. 播放卡顿/失真：下行 16k->24k 用最近邻 `resamplePcmNearest`，音质差；且无 jitter buffer，下行到达不均时 I2S DMA 欠载。
4. 唤醒后 `volc_afe_rx` 栈溢出：唤醒路径在 fetch 任务内调用 `startConversation()`（RTC join，调用栈很深），8KB 栈不够。此前 RESOLVED 那轮因 `GetRTCConfig` HTTP 500 从未真正建联，故该深路径未被走到。
5. task_wdt 触发：轮询用 `vTaskDelay(pdMS_TO_TICKS(5))`，而 `CONFIG_FREERTOS_HZ=100`（1 tick=10ms），5ms 向下取整为 0 tick = 不让出时间片，prio 5 任务空转打满 CPU 饿死 IDLE。
6. task_wdt 核间失衡：uplink 编码放在哪个核会饿死该核的 IDLE。core0=fetch+uplink / core1=capture(AFE)+play 的 2/2 分配时 wdt 为 0。

## Changes (volc_agent.cpp)
- 解耦上行：`afeFetchTask` 只 fetch + `enqueueUplink()` 入队 16k PCM 帧；新增 `volc_uplink` 任务做 Opus 编码 + `volc_send_audio_data`。fetch 不再被网络发送阻塞。
- 下行直出 24k：`initOpusCodec` 把 `esp_opus_dec_cfg_t.sample_rate` 设为 codec `output_sample_rate()`（24000），Opus 内部直接重采样，删除最近邻 `resamplePcmNearest` 及 `_opus_downlink_frame_samples`。
- 整包解码：`playbackTask` 用 `raw.consumed` 循环解出包内所有 20ms 帧并逐帧 `OutputData`。
- 抖动预缓冲：播放前先攒 `_playback_prime_frames=2` 帧再 drain（`flushPlaybackQueue` 重置 priming），配合 I2S 60ms DMA 缓冲吸收下行抖动。
- `volc_afe_rx` 栈 8192 -> 32768（PSRAM）。
- 轮询 `vTaskDelay` 由 5ms 改为 10ms（=1 tick，真正让出 CPU）。
- `volc_uplink` 固定到 core 0；与 `volc_play`(core1)/`volc_audio`(core1) 形成 2/2 核分配。
- `stop()` 增加 stop/wait `volc_uplink`。

## Verification (serial captures)
- 栈溢出修复后无 reboot：`volc_afe_rx` 32KB 栈后唤醒不再溢出。
- 上行采集完整：`afe uplink frames` 连续 1 -> 2650+/3500+/8000+ 无断点，与 feed frames 节奏对齐，无 `Ringbuffer full/empty`（仅唤醒瞬间一次 empty，属初始化窗口）。
- 解码全部成功：`opus decode failed` 计数为 0；下行 `audio_dl type=1` 正常到达解码。
- task_wdt：uplink 在 core 0 的最终版整段会话 **0 次** watchdog；core 1 版本会饿死 IDLE1（已回退）。
- 注：观察到一次 **独立的** Core 1 `LoadProhibited` 崩溃，发生在用户退出会话再重新进入 AI Agent 的第二次 `volc_agent::start()` -> `volc_create` -> SDK `liteInterrupterStartAsyncRead`（火山 SDK 内部线程创建）。与本次音频卡顿/采集修复无关，属 stop->start 重入路径的 SDK 状态问题（与历史 `engine is not in CREATED state` 同源），待单独排查。

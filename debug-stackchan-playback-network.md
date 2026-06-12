# debug-stackchan-playback-network

Status: [RESOLVED]

## Symptoms

- `AI.Agent` / StackChan 原生 Xiaozhi 链路播放明显卡顿。
- 使用 `VeRTC.Agent` 后再进入 `AI.Agent`，会卡住，随后提示需要连接网络。

## Hypotheses

- H1: VeRTC 先调用 `Hal::startNetwork()` 连上 WiFi 后，Xiaozhi `Application::Initialize()` 再调用 `Board::StartNetwork()` 时没有收到新的 `Connected` 事件，导致 activation 不启动，最终走到配网/网络提示。
- H2: Xiaozhi 下行音频包可能包含多个 Opus frame，但 `AudioService::OpusCodecTask()` 只解一次；剩余 `raw.consumed` 后的数据被丢掉，播放表现为断续/卡顿。
- H3: Claude 最新音量修复或视觉桥影响 StackChan 原生链路。代码检查未发现直接改动 Xiaozhi 播放队列；视觉桥仅在 VeRTC `_conversation_active` 时编码发送。

## Evidence

- `WifiBoard::StartNetwork()` 总是 `TryWifiConnect()`，即使 `WifiManager::IsConnected()` 已经为 true；已连接路径不会主动给新注册的 `network_event_callback_` 派发 `NetworkEvent::Connected`。
- `Application::Initialize()` 依赖 `MAIN_EVENT_NETWORK_CONNECTED` 才进入 `HandleNetworkConnectedEvent()` 并启动 `ActivationTask()`。
- `AppAiAgent` 中“Volc 后重启进 Xiaozhi”的保护只在同一个 App 实例的 `_volc_attempted` 生效；当前首页 `VeRTC.Agent` 和 `AI.Agent` 是两个独立实例，因此该保护覆盖不到用户的实际路径。
- `AudioService::OpusCodecTask()` 旧逻辑每个 `AudioStreamPacket` 只调用一次 `esp_opus_dec_decode()`，不循环消费 `raw.consumed` 后的剩余 payload。

## Fix

- `WifiBoard::StartNetwork()` 注册 callback 后，如果 WiFi 已连接，立即派发 `NetworkEvent::Connected` 并返回，不再重新 `StartStation()` 或启动连接超时定时器。
- `AudioService::OpusCodecTask()` 对单个下行 packet 循环解码，直到 `raw.len == 0`、decoder 出错、或播放队列已满。

## Verification

- 已重新生成 `firmware/patches/xiaozhi-esp32.patch`，并基于 `78/xiaozhi-esp32@v2.2.4` 执行 `git apply --check` 通过。
- `git diff --check -- . ':!firmware/patches/xiaozhi-esp32.patch'` 通过。
- `idf.py build` 通过，生成 `build/stackchan-volc-open.bin`。
- `idf.py -p /dev/cu.usbmodem2101 build flash` 通过，应用与 assets 分区写入校验成功并 hard reset。
- 串口 monitor 复位后启动正常：Launcher 打开，并创建 `VeRTC.Agent`、`AI.Agent`、`AVATAR` 等首页入口；未见启动阶段崩溃或网络配置卡死。
- 用户实测通过：`VeRTC.Agent -> 返回首页 -> AI.Agent` 重入正常，StackChan/Xiaozhi 下行播放连续性恢复。

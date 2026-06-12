/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "volc_agent.h"
#include <sdkconfig.h>
#include <audio/audio_codec.h>
#include <board.h>
#include <assets/lang_config.h>
#include <display/display.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <volc_conv_ai.h>
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <jpg/image_to_jpeg.h>
#include <apps/common/common.h>
#include <cJSON.h>
#include <assets.h>
#include <esp_mac.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>

#include <esp_afe_config.h>
#include <esp_afe_doa.h>
#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <esp_ae_rate_cvt.h>
#include <esp_ae_types.h>
#include <encoder/impl/esp_opus_enc.h>
#include <decoder/impl/esp_opus_dec.h>
#include <model_path.h>
#include <esp_timer.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

static const std::string_view _tag = "VOLC-Agent";

static std::mutex _mutex;
// 发送串行化锁：volc_send_* 在弱网/省电休眠时可能阻塞数百毫秒，绝不能持 _mutex
// 跨发送（UI 线程的 isRunning()/interrupt() 会被一并卡死，表现为整机"卡住"）。
// 加锁顺序固定为 _send_mutex → _mutex；stop() 销毁引擎前先取 _send_mutex，
// 保证在途发送完成后才 destroy。
static std::mutex _send_mutex;
static std::mutex _play_mutex;
static volc_engine_t _engine = nullptr;
static bool _running         = false;
// 引擎已 volc_start（RTC 会话已建联）。与 _running 区分：_running 表示 agent 任务
// 已起来（含 AFE 待机唤醒检测），_conv_started 表示真正与服务端建立了对话会话。
// 唤醒前 _running=true 但 _conv_started=false：只跑本地唤醒检测，不建联、不收发音频。
static std::atomic_bool _conv_started{false};
static std::atomic_bool _conversation_connecting{false};
static std::atomic_bool _connect_task_started{false};
static std::atomic_bool _audio_task_running{false};
static std::atomic_bool _audio_task_started{false};
static std::atomic_bool _video_task_running{false};
static std::atomic_bool _video_task_started{false};
// Async playback queue: keep SDK receive callbacks light; Opus decode and I2S
// writes run in a dedicated task to avoid overflowing VolcRTCMain stack.
static std::mutex _playback_mutex;
static std::deque<std::vector<uint8_t>> _playback_queue;
static std::atomic_bool _playback_task_running{false};
static std::atomic_bool _playback_task_started{false};
// 事件驱动唤醒：入队/停止时通知 playbackTask，避免队列空时固定 10ms 盲等导致
// 新包到达后晚取走、叠加网络抖动触发 DMA 欠载卡顿。
static TaskHandle_t _playback_task_handle = nullptr;
// 抖动预缓冲：服务端下行默认按 ~120ms 分包到达，且到达节奏不均。播放前先攒够
// _playback_prime_frames 个下行包再开始 drain，配合 I2S 60ms DMA 缓冲吸收抖动，
// 避免一来就播导致 DMA 欠载产生卡顿。仅在会话开始/flush 时预缓冲一次。
static std::atomic_bool _playback_priming{true};
// 服务端一轮回复会突发地把 TTS 音频快于实时推下来。32 包(~3.8s)偏小会丢音；
// 但 std::deque/std::vector 默认占内部 SRAM，256 包会把 minimal sram 压到危险水位。
// 96 包约 11s TTS，兼顾长回复缓冲和 CoreS3 内部 SRAM 安全。
constexpr size_t _playback_queue_max_frames = 96;
// 起播预缓冲 8 包（~160ms）：2 包（40ms）太薄，回答首包到达节奏稍有抖动就 DMA
// 欠载，表现为 TTS 开头卡顿；160ms 的起播延迟听感上可忽略。
constexpr size_t _playback_prime_frames = 8;

// Async uplink queue: audioCaptureTask enqueues 16kHz mono PCM frames; a
// dedicated uplink task does Opus encode + volc_send_audio_data. Decoupling
// prevents codec capture from being blocked by network send stalls.
static std::mutex _uplink_mutex;
static std::deque<std::vector<int16_t>> _uplink_queue;
static std::atomic_bool _uplink_task_running{false};
static std::atomic_bool _uplink_task_started{false};
static TaskHandle_t _uplink_task_handle = nullptr;
constexpr size_t _uplink_queue_max_frames = 50;  // ~1s of 20ms frames
// 诊断：上行队列丢帧计数 + 入队后峰值深度。确认 uplinkTask(core0) 编码是否跟不上
// 采集入队节奏导致丢帧(发送不稳定 -> 服务端 VAD 误判静音提前断句)。
static std::atomic<uint32_t> _uplink_drop_count{0};
static std::atomic<uint32_t> _uplink_peak_depth{0};

// Async tool-call queue: tool_calls 在 SDK 收包线程(VolcRTCMain)回调中到达，但端侧
// 工具可能长时间阻塞（如 shake_head 内的 vTaskDelay 数秒），在回调里同步执行会卡住
// 下行音频/消息收包造成播放断流甚至超时掉线。回调只把 payload 入队，独立任务解析
// 执行并回传结果。
static std::mutex _tool_mutex;
static std::deque<std::string> _tool_queue;
static std::atomic_bool _tool_task_running{false};
static std::atomic_bool _tool_task_started{false};
constexpr size_t _tool_queue_max = 8;

// AFE is only for local WakeNet at 16kHz. After wake, uplink bypasses AFE:
// codec raw 24kHz mic -> 16kHz mono -> Opus/RTC. This avoids using AFE output
// as ASR audio and keeps local VAD/NS/AGC out of the uplink path.
constexpr int _afe_sample_rate = 16000;
static const esp_afe_sr_iface_t* _afe_iface = nullptr;
static esp_afe_sr_data_t* _afe_data = nullptr;
static srmodel_list_t* _afe_models = nullptr;
static esp_ae_rate_cvt_handle_t _input_resampler  = nullptr; // codec input -> AFE 16K
static esp_ae_rate_cvt_handle_t _uplink_resampler = nullptr; // codec mic -> RTC uplink
static afe_doa_handle_t* _doa_handle = nullptr;
static void* _opus_encoder = nullptr;
static void* _opus_decoder = nullptr;
static int _opus_encoder_input_bytes = 0;
static int _opus_encoder_output_bytes = 0;
// 下行 RTC/Opus 协商 16k；解码器直出到 codec 输出采样率，避免 16k PCM 直接写入
// 24k I2S 导致播放变速。端侧不再做额外软件重采样。
static int _opus_decoder_output_rate = 16000;
static int _afe_mic_count = 0;
static std::atomic_bool _wake_source_valid{false};
static std::atomic<int> _wake_source_yaw{0};
static std::atomic<int> _wake_source_direction_x10{0};
static std::atomic<int64_t> _wake_source_update_us{0};
static std::atomic_bool _afe_fetch_running{false};
static std::atomic_bool _afe_fetch_started{false};

// 唤醒门控：进入链路后处于"待机等待唤醒"，本地 WakeNet 检测 "Hi Stack-Chan"。
// 唤醒前不上行音频（避免被服务端误当对话），唤醒后进入对话；对话期间下行静默
// 超过 _conv_idle_timeout_ms 自动回落到等待唤醒。
static std::atomic_bool _conversation_active{false};
// 工具优先灯色接管：set_led_color 工具点亮后，状态机灯色不再覆盖（否则 LLM 口头
// 确认进入 ANSWERING 的瞬间就刷掉工具刚设的颜色）。会话结束/工具熄灯时解除。
static std::atomic_bool _led_tool_override{false};
static std::atomic_bool _conv_wakenet_disabled{false};
// 最近一次会话活动（唤醒/下行音频/明显上行人声）的时间戳，用于空闲超时回待机。
static std::atomic<int64_t> _last_activity_us{0};
static std::atomic_bool _external_prompt_sent{false};
static std::atomic<int64_t> _external_prompt_last_attempt_us{0};
constexpr int64_t _conv_idle_timeout_ms = 15000;  // 对话静默 15s 回等待唤醒
constexpr uint32_t _uplink_active_rms = 600;      // 上行帧 RMS 超过此值视为用户在讲话

// 会话状态记录：仅用于 UI（灯色/表情/口型）与 idle timeout 判定，不再用于上行门控。
// 麦克风全程持续采集上行，不因任何状态丢帧。0 表示未进入对话；其余对应 volc_conv_status_e。
static std::atomic<int> _conv_status{0};

// 会话 UI 状态切换（定义在文件后部），afeFetchTask 唤醒/超时时需要调用。
void uiWaitingForWake();
void uiListening();
void uiSpeaking();
void sendExternalDatePromptIfNeeded();
void startPlaybackTask();
void startUplinkTask();
void stopUplinkTask();
void startToolTask();
void stopToolTask();
void deinitAfe();

// 会话建联/断联（定义在文件后部）。唤醒后才 volc_start 建联，空闲超时后 volc_stop
// 断联回待机，避免一进入链路服务端就主动推欢迎语开始对话。
bool startConversation();
void stopConversation();
void startConversationAsync();

constexpr int _volc_audio_sample_rate = 16000;
constexpr int _opus_uplink_frame_ms = 20;
constexpr int _opus_downlink_frame_ms = 20;
constexpr size_t _opus_uplink_frame_samples = _volc_audio_sample_rate * _opus_uplink_frame_ms / 1000;
constexpr int _opus_bitrate = 24000;
constexpr size_t _doa_window_frames = 1024;
// 低频视觉采集：2s/帧。软编码 QVGA JPEG 单帧 ~100-300ms CPU，0.5fps 下占空比
// <15%、prio 3 随时被音频任务抢占；帧副本与 JPEG 缓冲均在 PSRAM，对紧张的内部
// SRAM 无增量压力（V4L2 mmap 缓冲是板级初始化就常驻的，与视觉桥开关无关）。
constexpr int _video_frame_interval_ms = 2000;

// RTC opus 参数是 RTP 打包合同，不是编码器输入参数：Opus 的 RTP 时钟按 RFC 7587
// 恒为 48000，s_samples_per_frame 是每包时间戳步进（48k 时钟下 20ms 包 = 960），
// 与本地 PCM 采样率（16k）无关。曾被误改成 16000/320，时间戳走速变为正确值的
// 1/3，服务端 jitter buffer 按 48k 时钟收流时大量挤压丢弃——表现为"服务端收到的
// 人声被截断/变形、ASR 乱码且过早断句"，且端侧一切日志正常（drop=0）。
constexpr const char* _config_format = R"({
  "ver": 1,
  "iot": {
    "instance_id": "%s",
    "product_key": "%s",
    "product_secret": "%s",
    "device_name": "%s"
  },
  "rtc": {
    "log_level": 3,
    "audio": {
      "publish": true,
      "subscribe": true,
      "codec": 1
    },
    "video": {
      "publish": true,
      "subscribe": false,
      "codec": 3
    },
    "params": [
      "{\"audio\":{\"codec\":{\"opus\":{\"sample_rate\":48000,\"channels\":1,\"s_samples_per_frame\":960}}}}"
    ]
  }
})";

constexpr const char* _vision_enabled_params = R"({
  "Config": {
    "LLMConfig": {
      "VisionConfig": {
        "Enable": true
      }
    }
  }
})";

std::string getDeviceName()
{
    if (std::strlen(CONFIG_VOLC_DEVICE_NAME) > 0) {
        return CONFIG_VOLC_DEVICE_NAME;
    }

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[32] = {};
    std::snprintf(name, sizeof(name), "stackchan-%02x%02x%02x%02x%02x%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return name;
}

bool waitForSystemTime()
{
    time_t now = 0;
    std::time(&now);
    if (now > 1735689600) {
        return true;
    }

    if (!esp_sntp_enabled()) {
        // 与 Hal::startSntp 同一组服务器（需 CONFIG_LWIP_SNTP_MAX_SERVERS>=3）：
        // pool.ntp.org 在部分网络环境不可达，多服务器避免这里白等 10s。
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_setservername(2, "cn.pool.ntp.org");
        esp_sntp_init();
    }

    for (int i = 0; i < 100; ++i) {
        std::time(&now);
        if (now > 1735689600) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

bool initOpusCodec()
{
    if (_opus_encoder && _opus_decoder) {
        return true;
    }

    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc_cfg.sample_rate = _volc_audio_sample_rate;
    enc_cfg.channel = ESP_AUDIO_MONO;
    enc_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    enc_cfg.bitrate = _opus_bitrate;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    enc_cfg.complexity = 1;
    enc_cfg.enable_vbr = true;

    if (esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &_opus_encoder) != ESP_AUDIO_ERR_OK || !_opus_encoder) {
        mclog::tagError(_tag, "opus encoder open failed");
        _opus_encoder = nullptr;
        return false;
    }
    if (esp_opus_enc_get_frame_size(_opus_encoder,
                                    &_opus_encoder_input_bytes,
                                    &_opus_encoder_output_bytes) != ESP_AUDIO_ERR_OK ||
        _opus_encoder_input_bytes <= 0 || _opus_encoder_output_bytes <= 0) {
        mclog::tagError(_tag, "opus encoder frame size failed");
        esp_opus_enc_close(_opus_encoder);
        _opus_encoder = nullptr;
        _opus_encoder_input_bytes = 0;
        _opus_encoder_output_bytes = 0;
        return false;
    }

    esp_opus_dec_cfg_t dec_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    // RTC 下行按 16k Opus 协商；解码器直出 codec 播放采样率，避免 16k PCM
    // 直接写入 24k I2S 导致播放变速。上行独立按 16k 编码发送。
    auto playback_codec = Board::GetInstance().GetAudioCodec();
    _opus_decoder_output_rate = playback_codec ? playback_codec->output_sample_rate()
                                               : _volc_audio_sample_rate;
    dec_cfg.sample_rate = _opus_decoder_output_rate;
    dec_cfg.channel = ESP_AUDIO_MONO;
    dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    dec_cfg.self_delimited = false;
    if (esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &_opus_decoder) != ESP_AUDIO_ERR_OK || !_opus_decoder) {
        mclog::tagError(_tag, "opus decoder open failed");
        esp_opus_enc_close(_opus_encoder);
        _opus_encoder = nullptr;
        _opus_decoder = nullptr;
        _opus_encoder_input_bytes = 0;
        _opus_encoder_output_bytes = 0;
        return false;
    }

    mclog::tagInfo(_tag, "opus ready input_bytes={} output_bytes={}",
                   _opus_encoder_input_bytes, _opus_encoder_output_bytes);
    return true;
}

void deinitOpusCodec()
{
    if (_opus_encoder) {
        esp_opus_enc_close(_opus_encoder);
        _opus_encoder = nullptr;
    }
    if (_opus_decoder) {
        esp_opus_dec_close(_opus_decoder);
        _opus_decoder = nullptr;
    }
    _opus_encoder_input_bytes = 0;
    _opus_encoder_output_bytes = 0;
}

bool encodePcmToOpus(const int16_t* pcm, size_t samples, std::vector<uint8_t>& output)
{
    if (!_opus_encoder || !pcm || samples == 0) {
        output.clear();
        return false;
    }

    const size_t input_bytes = samples * sizeof(int16_t);
    if (input_bytes != static_cast<size_t>(_opus_encoder_input_bytes)) {
        mclog::tagWarn(_tag, "opus input size mismatch: {}", input_bytes);
        output.clear();
        return false;
    }

    output.resize(static_cast<size_t>(_opus_encoder_output_bytes));
    esp_audio_enc_in_frame_t in = {};
    in.buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm));
    in.len = static_cast<uint32_t>(input_bytes);

    esp_audio_enc_out_frame_t out = {};
    out.buffer = output.data();
    out.len = static_cast<uint32_t>(output.size());

    const esp_audio_err_t ret = esp_opus_enc_process(_opus_encoder, &in, &out);
    if (ret != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0) {
        mclog::tagWarn(_tag, "opus encode failed: {}", static_cast<int>(ret));
        output.clear();
        return false;
    }

    output.resize(out.encoded_bytes);
    return true;
}

bool sendAudioOpus(const uint8_t* data, size_t len)
{
    if (!data || len == 0) {
        return false;
    }

    // 只持 _send_mutex 跨网络发送；_mutex 仅短暂校验引擎存活。持有 _send_mutex
    // 期间 stop() 无法销毁引擎（其 destroy 前先取 _send_mutex），指针使用安全。
    std::lock_guard<std::mutex> send_lock(_send_mutex);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_running || !_engine) {
            return false;
        }
    }

    volc_audio_frame_info_t info = {};
    info.data_type = VOLC_AUDIO_DATA_TYPE_OPUS;
    info.commit    = false;
    const int ret = volc_send_audio_data(_engine, data, len, &info);
    if (ret != 0) {
        mclog::tagWarn(_tag, "audio uplink send failed: {}", ret);
    }
    return ret == 0;
}

void enqueueUplink(const int16_t* pcm, size_t samples)
{
    if (!pcm || samples == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(_uplink_mutex);
    if (_uplink_queue.size() >= _uplink_queue_max_frames) {
        // 网络长期发不出去时丢最旧帧，保证 fetch 侧入队不被无限堆积拖垮。
        _uplink_queue.pop_front();
        _uplink_drop_count.fetch_add(1);
    }
    _uplink_queue.emplace_back(pcm, pcm + samples);
    if (_uplink_queue.size() > _uplink_peak_depth.load()) {
        _uplink_peak_depth.store(_uplink_queue.size());
    }
    if (_uplink_task_handle) {
        xTaskNotifyGive(_uplink_task_handle);
    }
}

void uplinkTask(void*)
{
    std::vector<uint8_t> encoded;
    {
        std::lock_guard<std::mutex> lock(_uplink_mutex);
        _uplink_task_handle = xTaskGetCurrentTaskHandle();
    }
    while (_uplink_task_running.load()) {
        std::vector<int16_t> pcm;
        {
            std::lock_guard<std::mutex> lock(_uplink_mutex);
            if (!_uplink_queue.empty()) {
                pcm = std::move(_uplink_queue.front());
                _uplink_queue.pop_front();
            }
        }
        if (pcm.empty()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            continue;
        }
        if (encodePcmToOpus(pcm.data(), pcm.size(), encoded)) {
            sendAudioOpus(encoded.data(), encoded.size());
        }
        // 同优先级让渡（不能用 vTaskDelay(1)：HZ=100 下 1 tick=10ms，叠加编码+发送
        // 耗时后单帧处理超过 20ms 实时节拍，持续说话时队列必然积压，1s 后开始丢
        // 最旧帧——服务端表现为人声流断流/滞后被"截断"）。队列空时上方
        // ulTaskNotifyTake 已阻塞让出 CPU，这里 yield 仅用于防同核同优先级饿死。
        taskYIELD();
    }
    {
        std::lock_guard<std::mutex> lock(_uplink_mutex);
        _uplink_task_handle = nullptr;
    }
    _uplink_task_started.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void startUplinkTask()
{
    bool expected = false;
    if (!_uplink_task_started.compare_exchange_strong(expected, true)) {
        return;
    }
    _uplink_task_running.store(true);
    // 任务栈放 PSRAM，原因同 startAudioBridge。Opus 编码调用栈较深，
    // 之前 8KB 会在唤醒后首次编码时溢出。
    // 固定到 core 0：core 1 已跑 volc_audio(AFE/WakeNet 计算密集) + volc_play + SDK
    // 的 VolcRTCMain，再叠加 uplink 会饿死 IDLE1；core 0 仅 fetch(只入队，很轻)，
    // 把编码放这里达成 core0=fetch+uplink / core1=capture+play 的 2/2 均衡。
    if (xTaskCreatePinnedToCoreWithCaps(uplinkTask, "volc_uplink", 32768, nullptr, 5, nullptr, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        _uplink_task_running.store(false);
        _uplink_task_started.store(false);
        mclog::tagError(_tag, "failed to create uplink task");
    }
}

void stopUplinkTask()
{
    _uplink_task_running.store(false);
    std::lock_guard<std::mutex> lock(_uplink_mutex);
    _uplink_queue.clear();
    if (_uplink_task_handle) {
        xTaskNotifyGive(_uplink_task_handle);
    }
}

void enqueuePlayback(std::vector<uint8_t>&& opus)
{
    if (opus.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(_playback_mutex);
    if (_playback_queue.size() >= _playback_queue_max_frames) {
        _playback_queue.pop_front();
    }
    _playback_queue.emplace_back(std::move(opus));
    if (_playback_task_handle) {
        xTaskNotifyGive(_playback_task_handle);
    }
}

void playAudioOpus(const uint8_t* data, size_t len)
{
    if (!_opus_decoder || !data || len == 0) {
        return;
    }

    startPlaybackTask();
    enqueuePlayback(std::vector<uint8_t>(data, data + len));
}

void flushPlaybackQueue()
{
    std::lock_guard<std::mutex> lock(_playback_mutex);
    _playback_queue.clear();
    _playback_priming.store(true);
}

void playbackTask(void*)
{
    auto audio_codec = Board::GetInstance().GetAudioCodec();
    if (!audio_codec || !_opus_decoder) {
        _playback_task_started.store(false);
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    {
        // handle 的设置/清除/通知都在 _playback_mutex 内，防止 stop 侧对刚自删的
        // 任务句柄 xTaskNotifyGive。
        std::lock_guard<std::mutex> lock(_playback_mutex);
        _playback_task_handle = xTaskGetCurrentTaskHandle();
    }

    // 解码输出已是播放采样率（_opus_decoder_output_rate），无需再重采样。一个下行
    // 包可能含多个 20ms Opus 帧（服务端默认 ~120ms 分包），用 consumed 循环把整包解完。
    const size_t out_frame_samples =
        static_cast<size_t>(_opus_decoder_output_rate) * _opus_downlink_frame_ms / 1000;
    std::vector<uint8_t> pcm_bytes(out_frame_samples * sizeof(int16_t));
    std::vector<int16_t> playback_pcm;

    while (_playback_task_running.load()) {
        std::vector<uint8_t> opus;
        {
            std::lock_guard<std::mutex> lock(_playback_mutex);
            // 抖动预缓冲：攒够 prime 帧再开始播，吸收下行到达节奏抖动，避免 DMA 欠载卡顿。
            if (_playback_priming.load()) {
                if (_playback_queue.size() < _playback_prime_frames) {
                    // keep waiting for more frames
                } else {
                    _playback_priming.store(false);
                }
            }
            if (!_playback_priming.load() && !_playback_queue.empty()) {
                opus = std::move(_playback_queue.front());
                _playback_queue.pop_front();
            }
        }

        if (opus.empty()) {
            // 队列空/预缓冲未满：阻塞等待入队通知（入队 xTaskNotifyGive 唤醒），
            // 超时 10ms 兜底重查，避免固定盲等带来的取包延迟与 DMA 欠载。
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            continue;
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = opus.data();
        raw.len = static_cast<uint32_t>(opus.size());

        // 整包解码：每次解出一个 20ms 帧并立即写出，循环到本包数据消费完。
        while (raw.len > 0 && _playback_task_running.load()) {
            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = pcm_bytes.data();
            frame.len = static_cast<uint32_t>(pcm_bytes.size());

            esp_audio_dec_info_t dec_info = {};
            esp_audio_err_t ret = esp_opus_dec_decode(_opus_decoder, &raw, &frame, &dec_info);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && frame.needed_size > pcm_bytes.size()) {
                pcm_bytes.resize(frame.needed_size);
                frame.buffer = pcm_bytes.data();
                frame.len = static_cast<uint32_t>(pcm_bytes.size());
                ret = esp_opus_dec_decode(_opus_decoder, &raw, &frame, &dec_info);
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                mclog::tagWarn(_tag, "opus decode failed: {}", static_cast<int>(ret));
                break;
            }
            if (frame.decoded_size > 0) {
                playback_pcm.resize(frame.decoded_size / sizeof(int16_t));
                std::memcpy(playback_pcm.data(), pcm_bytes.data(),
                            playback_pcm.size() * sizeof(int16_t));

                std::lock_guard<std::mutex> play_lock(_play_mutex);
                if (!audio_codec->output_enabled()) {
                    audio_codec->EnableOutput(true);
                }
                audio_codec->OutputData(playback_pcm);
            }
            if (raw.consumed == 0) {
                // guard against a stuck decoder that reports no progress
                break;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
        }
    }

    {
        std::lock_guard<std::mutex> lock(_playback_mutex);
        _playback_task_handle = nullptr;
    }
    _playback_task_started.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void startPlaybackTask()
{
    bool expected = false;
    if (!_playback_task_started.compare_exchange_strong(expected, true)) {
        return;
    }

    _playback_task_running.store(true);
    // 绑 core 0：core 1 已被 RTC SDK 任务 + AFE 内部 feed/fetch 任务 + volc_audio 占满，
    // 播放解码若也在 core 1 会抢占 AFE 处理时间，导致 FEED ringbuffer 来不及排空而溢出
    // 丢麦克风数据（上行失真、ASR 识别不准）。挪到相对空闲的 core 0（仅 afe_rx/uplink，
    // 多处于阻塞/间歇态）消除核内争抢。
    if (xTaskCreatePinnedToCoreWithCaps(playbackTask, "volc_play", 32768, nullptr, 5, nullptr, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        _playback_task_running.store(false);
        _playback_task_started.store(false);
        mclog::tagError(_tag, "failed to create playback task");
    }
}

void stopPlaybackTask()
{
    _playback_task_running.store(false);
    // 清队列并唤醒可能正阻塞等通知的 playbackTask，使其尽快退出循环结束任务。
    std::lock_guard<std::mutex> lock(_playback_mutex);
    _playback_queue.clear();
    _playback_priming.store(true);
    if (_playback_task_handle) {
        xTaskNotifyGive(_playback_task_handle);
    }
}

// 从 assets 分区加载 WakeNet 模型（srmodels.bin），返回 srmodel_list_t*。
// 本项目无独立 "model" 分区，模型与 xiaozhi 共用 assets 分区，故复用 Assets
// 单例取出 index.json 指向的 srmodels.bin 子文件指针，再 srmodel_load。
srmodel_list_t* loadWakenetModels()
{
    auto& assets = Assets::GetInstance();
    if (!assets.partition_valid()) {
        mclog::tagError(_tag, "assets partition invalid, cannot load wakenet");
        return nullptr;
    }

    void* idx_ptr = nullptr;
    size_t idx_size = 0;
    if (!assets.GetAssetData("index.json", idx_ptr, idx_size)) {
        mclog::tagError(_tag, "index.json not found in assets");
        return nullptr;
    }

    cJSON* root = cJSON_ParseWithLength(static_cast<char*>(idx_ptr), idx_size);
    if (!root) {
        mclog::tagError(_tag, "index.json parse failed");
        return nullptr;
    }

    srmodel_list_t* models = nullptr;
    cJSON* srmodels = cJSON_GetObjectItem(root, "srmodels");
    if (cJSON_IsString(srmodels)) {
        void* sr_ptr = nullptr;
        size_t sr_size = 0;
        if (assets.GetAssetData(srmodels->valuestring, sr_ptr, sr_size)) {
            models = srmodel_load(static_cast<uint8_t*>(sr_ptr));
        } else {
            mclog::tagError(_tag, "srmodels file {} not found", srmodels->valuestring);
        }
    } else {
        mclog::tagError(_tag, "srmodels entry missing in index.json");
    }

    cJSON_Delete(root);
    return models;
}

bool initAfe(int input_sample_rate, int input_channels, bool input_reference)
{
    if (_afe_iface != nullptr && _afe_data != nullptr) {
        return true;
    }

    // Build "MMR"-style format: one M per microphone channel, optional R for the
    // playback reference. CoreS3 reports MIC1/MIC2 plus MIC3 reference when reference is on.
    std::string input_format;
    int mic_count = std::max(1, input_channels - (input_reference ? 1 : 0));
    _afe_mic_count = mic_count;
    for (int i = 0; i < mic_count; ++i) {
        input_format.push_back('M');
    }
    if (input_reference) {
        input_format.push_back('R');
    }

    // AFE_TYPE_SR（语音识别场景）而非 AFE_TYPE_VC：VC 类型内置非线性噪声抑制，
    // 在无真实回声参考时会把近端人声整段当噪声抹掉（实测 mic_rms=1300 -> out_rms=12）。
    // SR 类型只做线性 AEC，保留人声清晰度，把上行决策交给火山服务端。
    // 加载 WakeNet 模型（"Hi Stack-Chan"）用于本地唤醒门控。本项目模型不在独立
    // "model" 分区，而是与 xiaozhi 共用 assets 分区里的 srmodels.bin，
    // 故复用 Assets 单例（已 mmap assets 分区）取出 srmodels.bin 再 srmodel_load，
    // 与 xiaozhi 链路 (assets.cc:LoadSrmodelsFromIndex) 的加载方式一致。
    if (!_afe_models) {
        _afe_models = loadWakenetModels();
    }
    if (!_afe_models || _afe_models->num <= 0) {
        mclog::tagError(_tag, "wakenet model init failed");
        return false;
    }
    afe_config_t* afe_config =
        afe_config_init(input_format.c_str(), _afe_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afe_config) {
        mclog::tagError(_tag, "afe_config_init failed");
        return false;
    }

    // Memory optimization: keep AFE on PSRAM, disable everything that needs
    // model weights or extra DSP buffers. We only need linear AEC.
    // 回声消除交由火山服务端处理，本地关闭 AEC，保证近端人声原样上行不被误伤。
    // 双通道输入时 SE(BSS) 关闭后 AFE 只取第一路 mic 通道，等效透传麦克风。
    afe_config->aec_init     = false;
    afe_config->se_init      = false;
    afe_config->ns_init      = false;
    // VAD 关闭：上行决策由火山服务端做，本地 VAD 模型只是白吃 SRAM/CPU。
    afe_config->vad_init     = false;
    // WakeNet 开启：本地检测 "Hi Stack-Chan" 唤醒词，作为进入对话的门控。
    afe_config->wakenet_init = true;
    afe_config->agc_init     = false;
    afe_config->afe_perferred_core     = 1;
    afe_config->afe_perferred_priority = 5;
    afe_config->memory_alloc_mode      = AFE_MEMORY_ALLOC_MORE_PSRAM;

    _afe_iface = esp_afe_handle_from_config(afe_config);
    if (!_afe_iface) {
        mclog::tagError(_tag, "esp_afe_handle_from_config failed");
        afe_config_free(afe_config);
        return false;
    }

    _afe_data = _afe_iface->create_from_config(afe_config);
    afe_config_free(afe_config);
    if (!_afe_data) {
        mclog::tagError(_tag, "afe create_from_config failed");
        _afe_iface = nullptr;
        return false;
    }

    // 24K mic+ref -> 16K mic+ref before feeding AFE.
    if (input_sample_rate != _afe_sample_rate) {
        esp_ae_rate_cvt_cfg_t cfg = {};
        cfg.src_rate        = static_cast<uint32_t>(input_sample_rate);
        cfg.dest_rate       = static_cast<uint32_t>(_afe_sample_rate);
        cfg.channel         = static_cast<uint8_t>(std::max(1, input_channels));
        cfg.bits_per_sample = ESP_AE_BIT16;
        // complexity=1 + MEMORY 模式：把滤波系数表放 PSRAM，释放紧张的内部 SRAM；
        // 24K->16K 是窄带语音，听感差异可忽略。
        cfg.complexity      = 1;
        cfg.perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY;
        if (esp_ae_rate_cvt_open(&cfg, &_input_resampler) != ESP_AE_ERR_OK) {
            mclog::tagError(_tag, "input resampler open failed");
            _afe_iface->destroy(_afe_data);
            _afe_data  = nullptr;
            _afe_iface = nullptr;
            return false;
        }
    }

    if (input_sample_rate != _volc_audio_sample_rate) {
        esp_ae_rate_cvt_cfg_t cfg = {};
        cfg.src_rate        = static_cast<uint32_t>(input_sample_rate);
        cfg.dest_rate       = static_cast<uint32_t>(_volc_audio_sample_rate);
        cfg.channel         = 1;
        cfg.bits_per_sample = ESP_AE_BIT16;
        // 上行 24K->16K 下采样。complexity 保持 1：该重采样在 audioCaptureTask 里每个
        // 采集块都跑且固定 core 1（会话期与 volc_play+RTC SDK 抢核），提到 3 会让播放卡顿、
        // 采集帧时序抖动，且实测对 ASR 完整性无改善，故回退到与下行/AFE 一致的最低 CPU 档。
        cfg.complexity      = 1;
        cfg.perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY;
        if (esp_ae_rate_cvt_open(&cfg, &_uplink_resampler) != ESP_AE_ERR_OK) {
            mclog::tagError(_tag, "uplink resampler open failed");
            deinitAfe();
            return false;
        }
    }

    if (mic_count >= 2) {
        _doa_handle = afe_doa_create(input_format.c_str(), _afe_sample_rate, 20.0f, 0.06f, 1024);
        if (!_doa_handle) {
            mclog::tagWarn(_tag, "DOA init failed");
        }
    }

    // RTC SDK 上行只支持单声道（RTC 配置声明 channels:1 / 320 样本帧），上行链路
    // 假定 fetch 输出即 1 声道直接进 Opus 编码。这里显式校验：若换板/换 AFE 配置
    // 后 fetch 变多声道，交织数据会被当单声道上行（变调乱码），必须拒绝启动。
    const int fetch_ch = _afe_iface->get_fetch_channel_num(_afe_data);
    if (fetch_ch != 1) {
        mclog::tagError(_tag, "AFE fetch channels={} (uplink requires mono), abort", fetch_ch);
        deinitAfe();
        return false;
    }

    mclog::tagInfo(_tag,
                   "AFE ready (format={} feed_chunk={} fetch_chunk={} feed_ch={} fetch_ch={})",
                   input_format,
                   _afe_iface->get_feed_chunksize(_afe_data),
                   _afe_iface->get_fetch_chunksize(_afe_data),
                   _afe_iface->get_feed_channel_num(_afe_data),
                   _afe_iface->get_fetch_channel_num(_afe_data));
    return true;
}

void deinitAfe()
{
    if (_doa_handle) {
        afe_doa_destroy(_doa_handle);
        _doa_handle = nullptr;
    }
    _afe_mic_count = 0;
    _wake_source_valid.store(false);
    _wake_source_update_us.store(0);
    if (_input_resampler) {
        esp_ae_rate_cvt_close(_input_resampler);
        _input_resampler = nullptr;
    }
    if (_uplink_resampler) {
        esp_ae_rate_cvt_close(_uplink_resampler);
        _uplink_resampler = nullptr;
    }
    if (_afe_iface && _afe_data) {
        _afe_iface->destroy(_afe_data);
    }
    _afe_data  = nullptr;
    _afe_iface = nullptr;
    if (_afe_models) {
        esp_srmodel_deinit(_afe_models);
        _afe_models = nullptr;
    }
}

void audioCaptureTask(void*)
{
    auto audio_codec = Board::GetInstance().GetAudioCodec();
    if (!audio_codec) {
        mclog::tagError(_tag, "audio codec unavailable");
        _audio_task_started.store(false);
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    if (!_afe_iface || !_afe_data) {
        mclog::tagError(_tag, "AFE not initialized");
        _audio_task_started.store(false);
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    audio_codec->EnableInput(true);

    const int input_sample_rate = audio_codec->input_sample_rate();
    const int channels = std::max(1, audio_codec->input_channels());
    const int feed_chunk = _afe_iface->get_feed_chunksize(_afe_data);

    // Feed AFE in chunks of `feed_chunk` samples-per-channel; codec runs at
    // 24kHz so we read (feed_chunk * src_rate / 16000) frames, resample to
    // 16kHz, then hand exactly feed_chunk frames to the AFE.
    size_t src_frames_per_chunk = static_cast<size_t>(feed_chunk) *
                                  static_cast<size_t>(input_sample_rate) /
                                  static_cast<size_t>(_afe_sample_rate);
    if (src_frames_per_chunk == 0) {
        src_frames_per_chunk = static_cast<size_t>(feed_chunk);
    }

    // Ask the resampler how many output samples it may produce for this input
    // size. Without this margin, esp_ae_rate_cvt complains "output buffer too
    // small" because it reserves a few extra samples for its internal filter
    // tail and rounds up per channel.
    uint32_t resample_max_out_per_ch = static_cast<uint32_t>(feed_chunk);
    if (_input_resampler) {
        uint32_t hint = 0;
        if (esp_ae_rate_cvt_get_max_out_sample_num(
                _input_resampler,
                static_cast<uint32_t>(src_frames_per_chunk),
                &hint) == ESP_AE_ERR_OK && hint > resample_max_out_per_ch) {
            resample_max_out_per_ch = hint;
        }
    }
    resample_max_out_per_ch += 8; // safety margin

    uint32_t uplink_resample_max_out = static_cast<uint32_t>(_opus_uplink_frame_samples);
    if (_uplink_resampler) {
        uint32_t hint = 0;
        if (esp_ae_rate_cvt_get_max_out_sample_num(
                _uplink_resampler,
                static_cast<uint32_t>(src_frames_per_chunk),
                &hint) == ESP_AE_ERR_OK && hint > uplink_resample_max_out) {
            uplink_resample_max_out = hint;
        }
    }
    uplink_resample_max_out += 8;

    std::vector<int16_t> input_buf(src_frames_per_chunk * channels);
    std::vector<int16_t> resampled(resample_max_out_per_ch * channels);
    std::vector<int16_t> uplink_mono(src_frames_per_chunk);
    std::vector<int16_t> uplink_resampled(uplink_resample_max_out);
    std::vector<int16_t> uplink_accum;
    uplink_accum.reserve(_opus_uplink_frame_samples * 2);
    std::vector<int16_t> doa_window;
    doa_window.reserve(_doa_window_frames * channels);
    uint32_t feed_count = 0;
    uint32_t env_frames = 0;
    uint64_t env_sum_rms = 0;
    uint32_t env_max_rms = 0;

    while (_audio_task_running.load()) {
        if (!audio_codec->InputData(input_buf)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (_conversation_active.load()) {
            for (size_t i = 0; i < src_frames_per_chunk; ++i) {
                uplink_mono[i] = input_buf[i * channels];
            }

            const int16_t* uplink_ptr = uplink_mono.data();
            size_t uplink_samples = src_frames_per_chunk;
            bool uplink_ready = true;
            if (_uplink_resampler) {
                uint32_t out_samples = static_cast<uint32_t>(uplink_resampled.size());
                esp_ae_err_t err = esp_ae_rate_cvt_process(
                    _uplink_resampler,
                    uplink_mono.data(), static_cast<uint32_t>(src_frames_per_chunk),
                    uplink_resampled.data(), &out_samples);
                if (err == ESP_AE_ERR_OK) {
                    uplink_ptr = uplink_resampled.data();
                    uplink_samples = out_samples;
                } else {
                    uplink_ready = false;
                    if (feed_count == 0 || feed_count % 100 == 0) {
                        mclog::tagWarn(_tag, "uplink resample err={}", static_cast<int>(err));
                    }
                }
            }

            if (uplink_ready) {
                uplink_accum.insert(uplink_accum.end(), uplink_ptr, uplink_ptr + uplink_samples);
            }

            // 半双工门控，与指示灯逻辑严格一致：
            //   - ANSWERING（蓝灯，扬声器播 TTS）：停止采集上行，丢弃帧。CoreS3 无 AEC，
            //     若此期间继续上行，会把 TTS 回声当用户讲话满速上行，压垮发送链路并被
            //     服务端误判为"用户抢话"触发 INTERRUPTED（状态紊乱）。
            //   - LISTENING/THINKING/INTERRUPTED/ANSWER_FINISH 及待机：持续采集，不丢帧。
            // 命中门控时只丢弃、不入队，绝不发送静音帧——纯静音帧会被服务端 VAD 当作明确
            // 静音证据触发 endpoint，把用户半句话切走。
            const int conv = _conv_status.load();
            const bool model_busy = (conv == VOLC_CONV_STATUS_ANSWERING);

            while (uplink_accum.size() >= _opus_uplink_frame_samples) {
                if (model_busy) {
                    uplink_accum.erase(uplink_accum.begin(),
                                       uplink_accum.begin() + _opus_uplink_frame_samples);
                    continue;
                }

                // 直接上行原始重采样样本，不做软件增益。3.0x 数字增益会把底噪放大到
                // 持续可闻、把 TTS 回声/人声推到削波(±32768)失真，反而劣化 ASR。
                enqueueUplink(uplink_accum.data(), _opus_uplink_frame_samples);

                uint64_t out_sq = 0;
                int clip = 0;
                for (size_t i = 0; i < _opus_uplink_frame_samples; ++i) {
                    int32_t v = uplink_accum[i];
                    out_sq += static_cast<uint64_t>(v * v);
                    int32_t a = v < 0 ? -v : v;
                    if (a >= 32000) ++clip;
                }
                uint32_t out_rms = static_cast<uint32_t>(
                    std::sqrt(static_cast<double>(out_sq) / _opus_uplink_frame_samples));
                if (conv != VOLC_CONV_STATUS_THINKING &&
                    conv != VOLC_CONV_STATUS_ANSWERING && out_rms >= _uplink_active_rms) {
                    _last_activity_us.store(esp_timer_get_time());
                }

                env_sum_rms += out_rms;
                if (out_rms > env_max_rms) env_max_rms = out_rms;
                if (++env_frames >= 50) {
                    mclog::tagInfo(_tag,
                                   "uplink16 env avg={} max={} clip={} drop={} peak={} rs_out={}",
                                   env_sum_rms / env_frames, env_max_rms, clip,
                                   _uplink_drop_count.exchange(0), _uplink_peak_depth.exchange(0),
                                   uplink_samples);
                    env_frames = 0;
                    env_sum_rms = 0;
                    env_max_rms = 0;
                }

                uplink_accum.erase(uplink_accum.begin(),
                                   uplink_accum.begin() + _opus_uplink_frame_samples);
            }
        } else {
            uplink_accum.clear();
        }

        if (_conversation_active.load()) {
            continue;
        }

        const int16_t* feed_ptr = input_buf.data();
        size_t feed_frames = src_frames_per_chunk;

        if (_input_resampler) {
            uint32_t out_samples = static_cast<uint32_t>(resampled.size() / channels);
            esp_ae_err_t err = esp_ae_rate_cvt_process(
                _input_resampler,
                input_buf.data(), static_cast<uint32_t>(src_frames_per_chunk),
                resampled.data(), &out_samples);
            if (err != ESP_AE_ERR_OK) {
                if (feed_count == 0 || feed_count % 100 == 0) {
                    mclog::tagWarn(_tag, "input resample err={}", static_cast<int>(err));
                }
                continue;
            }
            feed_ptr = resampled.data();
            feed_frames = out_samples;
        }

        // AFE expects exactly feed_chunk samples per channel; if resampling
        // produced more/less, fall back to whatever we have (the AEC pipeline
        // tolerates small drift but we avoid sending undersized frames).
        if (feed_frames < static_cast<size_t>(feed_chunk)) {
            continue;
        }

        _afe_iface->feed(_afe_data, feed_ptr);
        ++feed_count;

        if (_doa_handle && _afe_mic_count >= 2 && channels >= 2) {
            const size_t doa_frames = std::min<size_t>(feed_frames, feed_chunk);
            const size_t doa_samples = doa_frames * channels;
            doa_window.insert(doa_window.end(), feed_ptr, feed_ptr + doa_samples);
            const size_t max_samples = _doa_window_frames * channels;
            if (doa_window.size() > max_samples) {
                doa_window.erase(doa_window.begin(), doa_window.end() - max_samples);
            }
            if (doa_window.size() == max_samples && feed_count % 5 == 0) {
                const float direction = afe_doa_process(_doa_handle, doa_window.data());
                if (std::isfinite(direction)) {
                    const int yaw = std::clamp(static_cast<int>(std::lround(direction - 90.0f)), -45, 45);
                    _wake_source_direction_x10.store(static_cast<int>(std::lround(direction * 10.0f)));
                    _wake_source_yaw.store(yaw);
                    _wake_source_update_us.store(esp_timer_get_time());
                    _wake_source_valid.store(true);
                }
            }
        }

        if (feed_count == 1 || feed_count % 500 == 0) {
            uint64_t mic_sq = 0, ref_sq = 0;
            const size_t probe = std::min<size_t>(src_frames_per_chunk, 256);
            // 诊断：统计两路通道逐样本相同的数量(same)与最大差值(maxdiff)，
            // 确认双麦是否读到冗余/同源数据(same≈probe 即两路完全相同)。
            size_t same = 0;
            int32_t maxdiff = 0;
            if (channels >= 2) {
                for (size_t i = 0; i < probe; ++i) {
                    int32_t m = input_buf[i * channels + 0];
                    int32_t r = input_buf[i * channels + channels - 1];
                    mic_sq += static_cast<uint64_t>(m * m);
                    ref_sq += static_cast<uint64_t>(r * r);
                    int32_t d = m - r;
                    if (d == 0) ++same;
                    if (d < 0) d = -d;
                    if (d > maxdiff) maxdiff = d;
                }
            } else {
                for (size_t i = 0; i < probe; ++i) {
                    int32_t m = input_buf[i];
                    mic_sq += static_cast<uint64_t>(m * m);
                }
            }
            uint32_t mic_rms = probe ? static_cast<uint32_t>(
                                  std::sqrt(static_cast<double>(mic_sq) / probe)) : 0;
            uint32_t ref_rms = probe ? static_cast<uint32_t>(
                                  std::sqrt(static_cast<double>(ref_sq) / probe)) : 0;
            mclog::tagInfo(_tag,
                           "afe feed frames={} chunk={} mic_rms={} ref_rms={} same={}/{} maxdiff={}",
                           feed_count, feed_chunk, mic_rms, ref_rms, same, probe, maxdiff);
        }
    }

    audio_codec->EnableInput(false);
    _audio_task_started.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void turnToWakeSource(const afe_fetch_result_t* res)
{
    const int64_t now_us = esp_timer_get_time();
    if (!_wake_source_valid.load() || now_us - _wake_source_update_us.load() > 1500 * 1000) {
        mclog::tagInfo(_tag, "wake source unavailable: trigger_channel={} raw_channels={} cached={}",
                       res ? res->trigger_channel_id : -1,
                       res ? res->raw_data_channels : 0,
                       static_cast<int>(_wake_source_valid.load()));
        return;
    }

    const int yaw = _wake_source_yaw.load();
    {
        LvglLockGuard lock;
        GetStackChan().motion().yawServo().moveWithSpeed(yaw * 10, 250);
    }
    mclog::tagInfo(_tag, "wake source direction={} yaw={}",
                   static_cast<float>(_wake_source_direction_x10.load()) / 10.0f,
                   yaw);
}

void afeFetchTask(void*)
{
    if (!_afe_iface || !_afe_data) {
        _afe_fetch_started.store(false);
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    while (_afe_fetch_running.load()) {
        if (_conversation_active.load()) {
            if (!_conv_wakenet_disabled.load()) {
                if (_afe_iface->disable_wakenet) {
                    _afe_iface->disable_wakenet(_afe_data);
                }
                _conv_wakenet_disabled.store(true);
                if (_afe_iface->reset_buffer) {
                    _afe_iface->reset_buffer(_afe_data);
                }
            }

            sendExternalDatePromptIfNeeded();

            const int64_t now_us = esp_timer_get_time();
            const int64_t idle_limit_ms =
                (_conv_status.load() == static_cast<int>(VOLC_CONV_STATUS_THINKING))
                    ? _conv_idle_timeout_ms
                    : _conv_idle_timeout_ms * 2;
            if (now_us - _last_activity_us.load() > idle_limit_ms * 1000) {
                mclog::tagInfo(_tag, "conversation idle timeout, back to wake-wait");
                _conversation_active.store(false);
                _conv_status.store(0);
                // 会话结束，解除工具灯色接管，待机灯色恢复状态机控制。
                _led_tool_override.store(false);
                stopConversation();
                if (_afe_iface->enable_wakenet) {
                    _afe_iface->enable_wakenet(_afe_data);
                }
                _conv_wakenet_disabled.store(false);
                if (_afe_iface->reset_buffer) {
                    _afe_iface->reset_buffer(_afe_data);
                }
                uiWaitingForWake();
                Board::GetInstance().GetDisplay()->SetChatMessage("system", "待连接");
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        afe_fetch_result_t* res =
            _afe_iface->fetch_with_delay(_afe_data, pdMS_TO_TICKS(100));
        if (!_afe_fetch_running.load()) {
            break;
        }
        if (!res || res->ret_value == ESP_FAIL || !res->data || res->data_size <= 0) {
            continue;
        }

        // 唤醒门控：检测到 "Hi Stack-Chan" 时建联并进入对话；丢弃唤醒词残留缓冲，
        // 避免把唤醒词本身当作首句话上行。
        if (res->wakeup_state == WAKENET_DETECTED) {
            if (!_conversation_active.load() && !_conversation_connecting.load()) {
                mclog::tagInfo(_tag, "wake word detected, starting conversation");
                turnToWakeSource(res);
                Board::GetInstance().GetDisplay()->SetChatMessage("system", "连接中");
                _conversation_connecting.store(true);
                startConversationAsync();
            }
            continue;
        }

        continue;
    }

    _afe_fetch_started.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void startAudioBridge()
{
    bool expected = false;
    if (!_audio_task_started.compare_exchange_strong(expected, true)) {
        return;
    }

    _audio_task_running.store(true);
    // 任务栈放 PSRAM：CoreS3 内部 SRAM 在 AFE 初始化后只剩 ~20KB 且高度碎片化，
    // 8KB 连续内部栈分配不出来（实测 xTaskCreatePinnedToCore 返回失败）。
    // 需配合 sdkconfig 的 FREERTOS_TASK_CREATE_ALLOW_EXT_MEM + SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY。
    if (xTaskCreatePinnedToCoreWithCaps(audioCaptureTask, "volc_audio", 8192, nullptr, 4, nullptr, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        _audio_task_running.store(false);
        _audio_task_started.store(false);
        mclog::tagError(_tag, "failed to create audio task");
    }
}

void stopAudioBridge()
{
    _audio_task_running.store(false);
}

void startAfeFetch()
{
    bool expected = false;
    if (!_afe_fetch_started.compare_exchange_strong(expected, true)) {
        return;
    }

    _afe_fetch_running.store(true);
    // 任务栈放 PSRAM，原因同 startAudioBridge。
    // 栈 32KB：唤醒时本任务内调用 startConversation()（RTC engine join，调用栈很深），
    // 8KB 会溢出（实测 volc_afe_rx stack overflow）。PSRAM 充裕，直接给足。
    if (xTaskCreatePinnedToCoreWithCaps(afeFetchTask, "volc_afe_rx", 32768, nullptr, 5, nullptr, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        _afe_fetch_running.store(false);
        _afe_fetch_started.store(false);
        mclog::tagError(_tag, "failed to create afe fetch task");
    }
}

void stopAfeFetch()
{
    _afe_fetch_running.store(false);
}

void videoCaptureTask(void*)
{
    uint32_t frame_count = 0;
    while (_video_task_running.load()) {
        vTaskDelay(pdMS_TO_TICKS(_video_frame_interval_ms));
        if (!_video_task_running.load()) {
            break;
        }
        // 仅会话期取帧上传：待机期不编码不发送（传感器/V4L2 流是板级常开的，
        // 这里省下的是编码 CPU 与上行带宽）。
        if (!_conversation_active.load()) {
            continue;
        }

        auto camera = hal_bridge::board_get_camera();
        if (!camera || !camera->StreamCaptures()) {
            continue;
        }

        const uint8_t* frame_data = camera->GetFrameData();
        size_t frame_size         = camera->GetFrameSize();
        int width                 = camera->GetFrameWidth();
        int height                = camera->GetFrameHeight();
        int format                = camera->GetFrameFormat();
        if (!frame_data || frame_size == 0) {
            continue;
        }

        const int64_t t0   = esp_timer_get_time();
        uint8_t* jpeg_data = nullptr;
        size_t jpeg_len    = 0;
        if (image_to_jpeg((uint8_t*)frame_data, frame_size, width, height, (v4l2_pix_fmt_t)format, 20,
                          &jpeg_data, &jpeg_len) &&
            jpeg_data) {
            const int64_t encode_ms = (esp_timer_get_time() - t0) / 1000;
            const bool sent         = volc_agent::sendVideoJpeg(jpeg_data, jpeg_len);
            free(jpeg_data);

            ++frame_count;
            // 每帧性能取证：编码耗时 + JPEG 体积 + 内部 SRAM 余量，用于回答
            // "MJPEG 采集是否造成性能不足"。持续 sram 走低或 encode 飙升即告警。
            mclog::tagInfo(_tag, "video frame#{} {}x{} jpeg={}B enc={}ms sent={} sram={}",
                           frame_count, width, height, jpeg_len, encode_ms, sent,
                           heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
    }

    _video_task_started.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void startVideoBridge()
{
    bool expected = false;
    if (!_video_task_started.compare_exchange_strong(expected, true)) {
        return;
    }

    _video_task_running.store(true);
    // 栈放 PSRAM（内部 SRAM 紧张，6KB 内部栈曾是禁用视觉桥的原因之一）；
    // 绑 core 0：core 1 跑 AFE/采集，JPEG 软编码（单帧 ~100-300ms）放 core 0
    // 的 IO 阻塞型任务堆里，prio 3 低于全部音频任务，不与拾音争抢。
    if (xTaskCreatePinnedToCoreWithCaps(videoCaptureTask, "volc_video", 16384, nullptr, 3, nullptr, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        _video_task_running.store(false);
        _video_task_started.store(false);
        mclog::tagError(_tag, "failed to create video task");
    }
}

void stopVideoBridge()
{
    _video_task_running.store(false);
}

bool hasRequiredConfig()
{
    return std::strlen(CONFIG_VOLC_INSTANCE_ID) > 0 && std::strlen(CONFIG_VOLC_PRODUCT_KEY) > 0 &&
           std::strlen(CONFIG_VOLC_PRODUCT_SECRET) > 0 && std::strlen(CONFIG_VOLC_BOT_ID) > 0;
}

void onVolcEvent(volc_engine_t, volc_event_t* event, void*)
{
    if (!event) {
        return;
    }

    switch (event->code) {
        case VOLC_EV_CONNECTED:
            mclog::tagInfo(_tag, "connected");
            break;
        case VOLC_EV_DISCONNECTED:
            mclog::tagInfo(_tag, "disconnected");
            break;
        case VOLC_EV_QUOTA_EXCEEDED:
            mclog::tagError(_tag, "quota exceeded");
            break;
        default:
            mclog::tagInfo(_tag, "event: {}", static_cast<int>(event->code));
            break;
    }
}

// ---- 会话 UI 状态机 ----------------------------------------------------
// 三种视觉状态，灯色/表情/口型由这里统一控制，避免 onConversationStatus 与
// onAudioData 各自 showRgbColor 造成灯色互相打架。
// 状态机灯色统一入口：工具接管期间（_led_tool_override）不写灯。
void statusRgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (_led_tool_override.load()) {
        return;
    }
    GetHAL().showRgbColor(r, g, b);
}
// 注意：SetStatus 内部会按字符串自行设灯（LISTENING=蓝、SPEAKING=蓝），所以
// 这里在 SetStatus 之后再 showRgbColor 覆盖成期望颜色作为唯一真相源。

void uiWaitingForWake()
{
    // 待机等待唤醒：中性表情 + 柔和绿灯，移除说话口型动画。
    auto* d = Board::GetInstance().GetDisplay();
    d->SetStatus(Lang::Strings::LISTENING);  // 复用其"移除 speaking 动画"的逻辑
    d->SetEmotion("neutral");
    statusRgb(0x00, 0x30, 0x10);
}

void uiListening()
{
    // 已唤醒、等待/聆听用户：中性表情 + 绿灯。
    auto* d = Board::GetInstance().GetDisplay();
    d->SetStatus(Lang::Strings::LISTENING);
    d->SetEmotion("neutral");
    statusRgb(0x00, 0x40, 0x00);
}

void uiSpeaking()
{
    // 助手说话：开心表情 + 口型动画 + 蓝灯。
    auto* d = Board::GetInstance().GetDisplay();
    d->SetStatus(Lang::Strings::SPEAKING);  // 添加 SpeakingModifier 口型动画
    d->SetEmotion("happy");
    statusRgb(0x00, 0x00, 0x40);
}

int sendBinaryJsonMessage(volc_engine_t engine, const char* magic, cJSON* root)
{
    if (!engine || !magic || std::strlen(magic) != 4 || !root) {
        return -1;
    }

    char* json = cJSON_PrintUnformatted(root);
    if (!json) {
        return -1;
    }

    const size_t json_len = std::strlen(json);
    std::string message;
    message.resize(json_len + 8);
    std::memcpy(message.data(), magic, 4);
    message[4] = static_cast<char>((json_len >> 24) & 0xff);
    message[5] = static_cast<char>((json_len >> 16) & 0xff);
    message[6] = static_cast<char>((json_len >> 8) & 0xff);
    message[7] = static_cast<char>(json_len & 0xff);
    std::memcpy(message.data() + 8, json, json_len);

    volc_message_info_t info = {};
    info.is_binary = true;
    // 与 sendAudioOpus 同一发送锁纪律：_send_mutex 跨发送、_mutex 短校验。
    // 注意：调用方不得持 _mutex 调用本函数（锁序固定 _send_mutex → _mutex）。
    int ret = -1;
    {
        std::lock_guard<std::mutex> send_lock(_send_mutex);
        bool engine_valid;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            engine_valid = _running && _engine && engine == _engine;
        }
        if (engine_valid) {
            ret = volc_send_message(_engine, message.data(), message.size(), &info);
        }
    }
    cJSON_free(json);
    return ret;
}

void sendExternalDatePromptIfNeeded()
{
    if (_external_prompt_sent.load()) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    const int64_t last_attempt_us = _external_prompt_last_attempt_us.load();
    if (last_attempt_us != 0 && now_us - last_attempt_us < 1000 * 1000) {
        return;
    }
    _external_prompt_last_attempt_us.store(now_us);

    bool expected = false;
    if (!_external_prompt_sent.compare_exchange_strong(expected, true)) {
        return;
    }
    if (!_engine || !_conv_started.load()) {
        _external_prompt_sent.store(false);
        return;
    }

    time_t now = 0;
    std::time(&now);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);

    char content[64] = {};
    std::snprintf(content, sizeof(content), "今天是%04d年%02d月%02d日",
                  local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "Command", "ExternalPromptsForLLM");
    cJSON_AddStringToObject(root, "Message", content);
    const int ret = sendBinaryJsonMessage(_engine, "ctrl", root);
    if (ret != 0) {
        _external_prompt_sent.store(false);
        mclog::tagWarn(_tag, "external date prompt send failed: {}", ret);
    } else {
        mclog::tagInfo(_tag, "external date prompt sent: {}", content);
    }

    cJSON_Delete(root);
}

void onConversationStatus(volc_engine_t, volc_conv_status_e status, void*)
{
    mclog::tagInfo(_tag, "[volc-cb] conv_status={}", static_cast<int>(status));
    if (!_conversation_active.load()) {
        return;
    }
    // 记录会话状态供上行门控判断：THINKING/ANSWERING 期间停采集，其余期间正常上行。
    _conv_status.store(static_cast<int>(status));
    // 任何状态流转都视为会话活跃，刷新空闲计时：避免模型思考/回复期间（无上行人声、
    // thinking 期亦无下行音频）被 idle timeout 误判断联。
    _last_activity_us.store(esp_timer_get_time());
    sendExternalDatePromptIfNeeded();
    switch (status) {
        case VOLC_CONV_STATUS_LISTENING:
            mclog::tagInfo(_tag, "listening");
            uiListening();
            break;
        case VOLC_CONV_STATUS_THINKING:
            mclog::tagInfo(_tag, "thinking");
            Board::GetInstance().GetDisplay()->SetEmotion("doubtful");
            statusRgb(0x40, 0x30, 0x00);
            break;
        case VOLC_CONV_STATUS_ANSWERING:
            mclog::tagInfo(_tag, "answering");
            uiSpeaking();
            break;
        case VOLC_CONV_STATUS_INTERRUPTED:
            mclog::tagInfo(_tag, "interrupted");
            flushPlaybackQueue();
            // Drop any echo frames already accumulated inside AFE so we don't
            // keep streaming residual TTS energy back upstream after a barge-in.
            if (_afe_iface && _afe_data) {
                _afe_iface->reset_buffer(_afe_data);
            }
            uiListening();
            break;
        case VOLC_CONV_STATUS_ANSWER_FINISH:
            mclog::tagInfo(_tag, "answer finished");
            uiListening();
            break;
        default:
            break;
    }
}

void onAudioData(volc_engine_t, const void* data, size_t len, volc_audio_frame_info_t* info, void*)
{
    if (!data || len == 0 || !info) {
        return;
    }
    static uint32_t _dl_count = 0;
    if (_dl_count == 0 || _dl_count % 50 == 0) {
        mclog::tagInfo(_tag, "[volc-cb] audio_dl bytes={} type={}",
                       len, static_cast<int>(info->data_type));
    }
    ++_dl_count;

    if (info->data_type == VOLC_AUDIO_DATA_TYPE_OPUS) {
        _last_activity_us.store(esp_timer_get_time());
        playAudioOpus(static_cast<const uint8_t*>(data), len);
        return;
    }

    mclog::tagWarn(_tag, "unsupported audio received: {} bytes, type {}", len, static_cast<int>(info->data_type));
}

void onVideoData(volc_engine_t, const void*, size_t len, volc_video_frame_info_t* info, void*)
{
    mclog::tagInfo(_tag, "video received: {} bytes, type {}", len,
                   info ? static_cast<int>(info->data_type) : 0);
}

int jsonInt(cJSON* root, const char* key, int default_value)
{
    cJSON* value = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(value) ? value->valueint : default_value;
}

bool jsonBool(cJSON* root, const char* key, bool default_value)
{
    cJSON* value = cJSON_GetObjectItem(root, key);
    return cJSON_IsBool(value) ? cJSON_IsTrue(value) : default_value;
}

const char* jsonString(cJSON* root, const char* key, const char* default_value)
{
    cJSON* value = cJSON_GetObjectItem(root, key);
    const char* text = cJSON_GetStringValue(value);
    return text ? text : default_value;
}

std::string dispatchTool(const char* tool_name, const char* args_json)
{
    cJSON* args = cJSON_Parse(args_json && args_json[0] ? args_json : "{}");
    if (!args) {
        return R"({"ok":false,"error":"invalid_arguments"})";
    }

    std::string result = R"({"ok":false,"error":"unknown_tool"})";
    if (std::strcmp(tool_name, "self.robot.get_head_angles") == 0) {
        LvglLockGuard lock;
        auto& motion = GetStackChan().motion();
        result = fmt::format(R"({{"yaw":{},"pitch":{}}})",
                             motion.yawServo().getCurrentAngle() / 10,
                             motion.pitchServo().getCurrentAngle() / 10);
    } else if (std::strcmp(tool_name, "self.robot.set_head_angles") == 0) {
        int yaw = jsonInt(args, "yaw", -9999);
        int pitch = jsonInt(args, "pitch", -9999);
        int speed = std::clamp(jsonInt(args, "speed", 150), 100, 1000);

        LvglLockGuard lock;
        auto& motion = GetStackChan().motion();
        if (yaw != -9999) {
            motion.yawServo().moveWithSpeed(std::clamp(yaw, -128, 128) * 10, speed);
        }
        if (pitch != -9999) {
            motion.pitchServo().moveWithSpeed(std::clamp(pitch, 0, 90) * 10, speed);
        }
        result = R"({"ok":true})";
    } else if (std::strcmp(tool_name, "self.robot.shake_head") == 0) {
        const int times = std::clamp(jsonInt(args, "times", 2), 1, 5);
        const int amplitude = std::clamp(jsonInt(args, "amplitude", 25), 5, 60);
        const int speed = std::clamp(jsonInt(args, "speed", 250), 100, 1000);
        const int pause_ms = std::clamp(jsonInt(args, "pause_ms", 180), 50, 500);
        for (int i = 0; i < times; ++i) {
            {
                LvglLockGuard lock;
                GetStackChan().motion().yawServo().moveWithSpeed(-amplitude * 10, speed);
            }
            vTaskDelay(pdMS_TO_TICKS(pause_ms));
            {
                LvglLockGuard lock;
                GetStackChan().motion().yawServo().moveWithSpeed(amplitude * 10, speed);
            }
            vTaskDelay(pdMS_TO_TICKS(pause_ms));
        }
        {
            LvglLockGuard lock;
            GetStackChan().motion().yawServo().moveWithSpeed(0, speed);
        }
        result = R"({"ok":true})";
    } else if (std::strcmp(tool_name, "self.robot.set_led_color") == 0) {
        int red = std::clamp(jsonInt(args, "red", 0), 0, 168);
        int green = std::clamp(jsonInt(args, "green", 0), 0, 168);
        int blue = std::clamp(jsonInt(args, "blue", 0), 0, 168);

        // 工具点亮期间阻止状态机灯色覆盖（否则 LLM 口头确认进 ANSWERING 的瞬间
        // statusRgb 就把颜色刷掉）；全 0 熄灯则交还状态机控制。
        _led_tool_override.store(!(red == 0 && green == 0 && blue == 0));
        {
            LvglLockGuard lock;
            GetStackChan().leftNeonLight().setColor(red, green, blue);
            GetStackChan().rightNeonLight().setColor(red, green, blue);
        }
        // 同步直写硬件立即生效（NeonLight 走 UI 泵逐帧渐变，留作动画收尾）。
        GetHAL().showRgbColor(red, green, blue);
        mclog::tagInfo(_tag, "set_led_color applied r={} g={} b={}", red, green, blue);
        result = R"({"ok":true})";
    } else if (std::strcmp(tool_name, "self.robot.create_reminder") == 0) {
        int duration_seconds = std::clamp(jsonInt(args, "duration_seconds", 60), 1, 86400);
        const char* message = jsonString(args, "message", "Time's up!");
        bool repeat = jsonBool(args, "repeat", false);
        int id = tools::create_reminder(duration_seconds * 1000, message, repeat);
        result = fmt::format(R"({{"id":{}}})", id);
    } else if (std::strcmp(tool_name, "self.robot.get_reminders") == 0) {
        cJSON* reminders_json = cJSON_CreateArray();
        for (const auto& reminder : tools::get_active_reminders()) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", reminder.id);
            cJSON_AddNumberToObject(item, "duration_ms", reminder.durationMs);
            cJSON_AddStringToObject(item, "message", reminder.message.c_str());
            cJSON_AddBoolToObject(item, "repeat", reminder.repeat);
            cJSON_AddItemToArray(reminders_json, item);
        }
        char* json = cJSON_PrintUnformatted(reminders_json);
        result = json ? json : "[]";
        cJSON_free(json);
        cJSON_Delete(reminders_json);
    } else if (std::strcmp(tool_name, "self.robot.stop_reminder") == 0) {
        int id = jsonInt(args, "id", -1);
        tools::stop_reminder(id);
        result = R"({"ok":true})";
    } else if (std::strcmp(tool_name, "self.robot.get_volume") == 0) {
        auto codec = Board::GetInstance().GetAudioCodec();
        int volume = codec ? codec->output_volume() : 0;
        result = fmt::format(R"({{"volume":{}}})", volume);
    } else if (std::strcmp(tool_name, "self.robot.set_volume") == 0) {
        int volume = std::clamp(jsonInt(args, "volume", 70), 0, 100);
        auto codec = Board::GetInstance().GetAudioCodec();
        int old_volume = -1;
        if (codec) {
            old_volume = codec->output_volume();
            codec->SetOutputVolume(volume);
        }
        mclog::tagInfo(_tag, "set_volume applied {} -> {}", old_volume, volume);
        result = fmt::format(R"({{"ok":true,"volume":{}}})", volume);
    }

    cJSON_Delete(args);
    return result;
}

void sendToolResult(volc_engine_t engine, const char* call_id, const std::string& content)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ToolCallID", call_id ? call_id : "");
    cJSON_AddStringToObject(root, "Content", content.c_str());

    // 引擎存活校验在 sendBinaryJsonMessage 内部完成（_send_mutex/_mutex 锁序），
    // 这里不能持 _mutex 调用，否则与其内部加锁顺序冲突。
    const int ret = sendBinaryJsonMessage(engine, "func", root);
    mclog::tagInfo(_tag, "tool result sent ret={} id={}", ret, call_id ? call_id : "");

    cJSON_Delete(root);
}

void handleToolMessage(volc_engine_t engine, cJSON* root)
{
    cJSON* tool_calls = cJSON_GetObjectItem(root, "tool_calls");
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, tool_calls) {
        cJSON* id = cJSON_GetObjectItem(item, "id");
        cJSON* function = cJSON_GetObjectItem(item, "function");
        cJSON* name = function ? cJSON_GetObjectItem(function, "name") : nullptr;
        cJSON* arguments = function ? cJSON_GetObjectItem(function, "arguments") : nullptr;
        const char* call_id = cJSON_GetStringValue(id);
        const char* tool_name = cJSON_GetStringValue(name);
        // arguments 官方协议是 JSON 字符串，但服务端也可能直接发 JSON 对象。
        // 若按字符串取失败则序列化对象——否则所有参数静默落到默认值，工具"执行
        // 成功"却无实际效果（音量默认 70、LED 全 0、转头 -9999 不动）。
        const char* args_json = cJSON_GetStringValue(arguments);
        char* args_owned = nullptr;
        if (!args_json && arguments &&
            (cJSON_IsObject(arguments) || cJSON_IsArray(arguments))) {
            args_owned = cJSON_PrintUnformatted(arguments);
            args_json = args_owned;
        }
        if (!call_id || !tool_name) {
            cJSON_free(args_owned);
            continue;
        }

        mclog::tagInfo(_tag, "tool call: {} args={}", tool_name, args_json ? args_json : "{}");

        // 字幕显示调用过程：执行前提示、执行后报结果。短名去掉 self.robot. 前缀。
        // 注意只用全字体确定包含的字符（[工具] 等常用字），勿用 ⚙ 等特殊符号
        // （字体缺字形会静默不渲染）。
        const char* short_name =
            std::strncmp(tool_name, "self.robot.", 11) == 0 ? tool_name + 11 : tool_name;
        Board::GetInstance().GetDisplay()->SetChatMessage(
            "system", fmt::format("[工具] {} 执行中", short_name).c_str());

        const std::string tool_result = dispatchTool(tool_name, args_json);
        mclog::tagInfo(_tag, "tool result: {}", tool_result);
        sendToolResult(engine, call_id, tool_result);

        const bool ok = tool_result.find("\"error\"") == std::string::npos;
        Board::GetInstance().GetDisplay()->SetChatMessage(
            "system", fmt::format("[工具] {} {}", short_name, ok ? "完成" : "失败").c_str());

        cJSON_free(args_owned);
    }
}

void toolDispatchTask(void*)
{
    while (_tool_task_running.load()) {
        std::string payload;
        {
            std::lock_guard<std::mutex> lock(_tool_mutex);
            if (!_tool_queue.empty()) {
                payload = std::move(_tool_queue.front());
                _tool_queue.pop_front();
            }
        }
        if (payload.empty()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
        if (!root) {
            mclog::tagWarn(_tag, "tool message json parse failed");
            continue;
        }
        // _engine 在 stop() 等待本任务退出后才 destroy，这里使用是安全的。
        handleToolMessage(_engine, root);
        cJSON_Delete(root);
    }
    _tool_task_started.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void startToolTask()
{
    bool expected = false;
    if (!_tool_task_started.compare_exchange_strong(expected, true)) {
        return;
    }
    _tool_task_running.store(true);
    // 任务栈放 PSRAM，原因同 startAudioBridge。工具执行会拿 LVGL 锁并做舵机动作，
    // 低优先级 + core 0 即可，不与音频管线抢核。
    if (xTaskCreatePinnedToCoreWithCaps(toolDispatchTask, "volc_tool", 8192, nullptr, 3, nullptr, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        _tool_task_running.store(false);
        _tool_task_started.store(false);
        mclog::tagError(_tag, "failed to create tool task");
    }
}

void stopToolTask()
{
    _tool_task_running.store(false);
    std::lock_guard<std::mutex> lock(_tool_mutex);
    _tool_queue.clear();
}

void handleSubtitleMessage(cJSON* root)
{
    cJSON* type = cJSON_GetObjectItem(root, "type");
    const char* type_value = cJSON_GetStringValue(type);
    if (!type_value || std::strcmp(type_value, "subtitle") != 0) {
        return;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data)) {
        return;
    }

    // 字幕语义对齐官方参考实现（volc_conv.c __on_subtitle_message_received）：
    // text 为本句累积全文，definite=true 表示本句定稿；sequence 只是分包序号，
    // RTS 可靠有序投递下无需去重，逐帧覆盖渲染即可。此前按 sequence 做"迟到帧"
    // 丢弃，当服务端序号语义与假设不符（跨句不重置等）时把同句新帧误判为旧帧
    // 丢掉，气泡停在旧文本上，表现为 TTS/ASR 字幕缺字。
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, data) {
        cJSON* text = cJSON_GetObjectItem(item, "text");
        const char* value = cJSON_GetStringValue(text);
        cJSON* user_id = cJSON_GetObjectItem(item, "userId");
        const char* uid = cJSON_GetStringValue(user_id);
        if (!value || !value[0]) {
            continue;
        }

        // 按 userId 区分角色：bot* 是下行 TTS（assistant），其余是上行用户语音（user）。
        const bool is_bot = uid && std::strncmp(uid, "bot", 3) == 0;
        Board::GetInstance().GetDisplay()->SetChatMessage(is_bot ? "assistant" : "user", value);

        // 定稿帧（每句一条）打印全文，便于核对字幕完整性；增量帧不打日志避免刷屏。
        if (cJSON_IsTrue(cJSON_GetObjectItem(item, "definite"))) {
            mclog::tagInfo(_tag, "subtitle[{}] {}", is_bot ? "bot" : "user", value);
        }
    }
}

void onMessageData(volc_engine_t, const void* data, size_t len, volc_message_info_t*, void*)
{
    if (!data || len == 0) {
        return;
    }

    const char* text = static_cast<const char*>(data);
    if (len > 8 && (std::memcmp(text, "tool", 4) == 0 || std::memcmp(text, "subv", 4) == 0 ||
                    std::memcmp(text, "info", 4) == 0)) {
        const size_t payload_len =
            (static_cast<size_t>(static_cast<uint8_t>(text[4])) << 24) |
            (static_cast<size_t>(static_cast<uint8_t>(text[5])) << 16) |
            (static_cast<size_t>(static_cast<uint8_t>(text[6])) << 8) |
            static_cast<size_t>(static_cast<uint8_t>(text[7]));
        if (payload_len == 0 || payload_len > len - 8) {
            mclog::tagWarn(_tag, "message payload length invalid: {}", payload_len);
            return;
        }
        std::string payload(text + 8, payload_len);

        // 任何结构化消息（字幕/工具调用）都视为会话活跃，刷新空闲计时。与官方
        // 参考实现一致（volc_conv.c 每收到字幕即清零 wait_time）：用户说话期间
        // ASR 增量字幕持续到达，靠它续命；此前只认"上行 RMS>600"，用户声音稍轻
        // 就会在说话途中被 idle timeout 误断会话（采集中断 + 识别不全 + 退回待机）。
        _last_activity_us.store(esp_timer_get_time());

        if (std::memcmp(text, "tool", 4) == 0) {
            // 工具执行可能长时间阻塞（舵机动作等），不能占住 SDK 收包线程，
            // 入队交给 volc_tool 任务执行并回传结果。
            std::lock_guard<std::mutex> lock(_tool_mutex);
            if (_tool_queue.size() >= _tool_queue_max) {
                _tool_queue.pop_front();
            }
            _tool_queue.emplace_back(std::move(payload));
            return;
        }

        if (std::memcmp(text, "info", 4) == 0) {
            // 官方协议的 function_calling 触发事件（function_call_service.c）：
            // {"event_type":"function_calling","function":"...","tool_call_id":"..."}。
            // 它先于 tool 消息到达，用作字幕上最早的"调用中"提示。
            cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
            if (root) {
                const char* ev = jsonString(root, "event_type", "");
                const char* fn = jsonString(root, "function", "");
                if (std::strcmp(ev, "function_calling") == 0 && fn[0]) {
                    const char* short_name =
                        std::strncmp(fn, "self.robot.", 11) == 0 ? fn + 11 : fn;
                    mclog::tagInfo(_tag, "function_calling triggered: {}", fn);
                    Board::GetInstance().GetDisplay()->SetChatMessage(
                        "system", fmt::format("[工具] {} 调用中", short_name).c_str());
                }
                cJSON_Delete(root);
            }
            return;
        }

        mclog::tagDebug(_tag, "subv payload: {}", payload.c_str());
        cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
        if (!root) {
            mclog::tagWarn(_tag, "message json parse failed");
            return;
        }
        handleSubtitleMessage(root);
        cJSON_Delete(root);
    } else if (len > 8 && std::isalnum(static_cast<unsigned char>(text[0])) &&
               std::isalnum(static_cast<unsigned char>(text[1])) &&
               std::isalnum(static_cast<unsigned char>(text[2])) &&
               std::isalnum(static_cast<unsigned char>(text[3]))) {
        // 未识别的 binary magic：打日志而非静默丢弃，排查云端用了哪个通道投递
        // （已知 magic：tool/subv/info/func/ctrl/conv）。
        mclog::tagWarn(_tag, "unhandled message magic '{}{}{}{}' len={}",
                       text[0], text[1], text[2], text[3], len);
    } else if (std::memchr(text, 0, len) == nullptr) {
        std::string message(text, len);
        Board::GetInstance().GetDisplay()->SetChatMessage("assistant", message.c_str());
    }
}

// 检测到唤醒词后建联：volc_start 启动 RTC 会话，服务端开始推欢迎语/对话。
// 已建联则直接返回 true（幂等）。afeFetchTask 持有的执行流不持有 _mutex，这里加锁安全。
bool startConversation()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_running || !_engine) {
        return false;
    }
    if (_conv_started.load()) {
        return true;
    }

    volc_opt_t opt = {};
    opt.mode       = VOLC_MODE_RTC;
    opt.bot_id     = const_cast<char*>(CONFIG_VOLC_BOT_ID);
    opt.params     = const_cast<char*>(_vision_enabled_params);

    int ret = volc_start(_engine, &opt);
    if (ret != 0) {
        mclog::tagError(_tag, "volc_start failed: {}", ret);
        return false;
    }
    _conv_started.store(true);
    _external_prompt_sent.store(false);
    _external_prompt_last_attempt_us.store(0);
    return true;
}

void conversationConnectTask(void*)
{
    const bool ok = startConversation();
    if (ok && _afe_fetch_running.load()) {
        _conversation_active.store(true);
        _conv_status.store(static_cast<int>(VOLC_CONV_STATUS_LISTENING));
        _last_activity_us.store(esp_timer_get_time());
        Board::GetInstance().GetDisplay()->SetChatMessage("system", "连接成功");
        uiListening();
    } else {
        mclog::tagWarn(_tag, "startConversation failed, stay in wake-wait");
        Board::GetInstance().GetDisplay()->SetChatMessage("system", "待连接");
    }
    _conversation_connecting.store(false);
    _connect_task_started.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void startConversationAsync()
{
    bool expected = false;
    if (!_connect_task_started.compare_exchange_strong(expected, true)) {
        return;
    }
    if (xTaskCreatePinnedToCoreWithCaps(conversationConnectTask, "volc_connect", 32768, nullptr, 4, nullptr, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        _connect_task_started.store(false);
        _conversation_connecting.store(false);
        mclog::tagError(_tag, "failed to create connect task");
    }
}

// 空闲超时回待机：volc_stop 断开 RTC 会话，停止服务端下行；引擎保留以便下次唤醒重连。
void stopConversation()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_engine || !_conv_started.load()) {
        return;
    }
    mclog::tagInfo(_tag, "volc_stop (idle, back to wake-wait)");
    volc_stop(_engine);
    _conv_started.store(false);
    _external_prompt_sent.store(false);
    _external_prompt_last_attempt_us.store(0);
}

}  // namespace

namespace volc_agent {

bool start()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_running) {
            return true;
        }
    }

    if (!hasRequiredConfig()) {
        mclog::tagError(_tag, "missing volc config");
        return false;
    }

    // SNTP 等待最长 10s，放在锁外执行：持 _mutex 等待会卡住 UI 线程的
    // isRunning()/interrupt() 调用，表现为进入链路期间整机冻住。
    if (!waitForSystemTime()) {
        mclog::tagWarn(_tag, "system time not synced before volc_start");
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (_running) {
        return true;
    }

    const std::string device_name = getDeviceName();
    char config[1024] = {};
    int written       = std::snprintf(config, sizeof(config), _config_format, CONFIG_VOLC_INSTANCE_ID,
                                      CONFIG_VOLC_PRODUCT_KEY, CONFIG_VOLC_PRODUCT_SECRET, device_name.c_str());
    if (written <= 0 || written >= static_cast<int>(sizeof(config))) {
        mclog::tagError(_tag, "config too large");
        return false;
    }

    volc_event_handler_t handler = {};
    handler.on_volc_event = onVolcEvent;
    handler.on_volc_conversation_status = onConversationStatus;
    handler.on_volc_audio_data = onAudioData;
    handler.on_volc_video_data = onVideoData;
    handler.on_volc_message_data = onMessageData;

    int ret = volc_create(&_engine, config, &handler, nullptr);
    if (ret != 0) {
        mclog::tagError(_tag, "volc_create failed: {}", ret);
        _engine = nullptr;
        return false;
    }

    // 注意：RTC 会话(volc_start)推迟到检测到唤醒词后由 startConversation() 建联，
    // 避免一进入链路服务端就主动推欢迎语开始对话。这里只创建引擎并起 AFE 待机检测。
    _running = true;

    // 初始进入"等待唤醒"态：唤醒前不建联、不上行音频，仅跑本地 WakeNet 检测。
    _conversation_active.store(false);
    _conversation_connecting.store(false);
    _conv_started.store(false);
    _conv_wakenet_disabled.store(false);
    _external_prompt_sent.store(false);
    _external_prompt_last_attempt_us.store(0);
    _last_activity_us.store(0);

    if (!initOpusCodec()) {
        mclog::tagError(_tag, "Opus codec init failed");
        volc_destroy(_engine);
        _engine  = nullptr;
        _running = false;
        return false;
    }

    // Bring AFE up before the capture/fetch tasks so they can rely on the
    // iface/handle being valid. If AFE init fails (e.g. PSRAM exhausted) we
    // tear the engine back down rather than fall back to the old half-duplex
    // path -- the caller explicitly disallowed software gating.
    auto audio_codec = Board::GetInstance().GetAudioCodec();
    const int input_sr   = audio_codec ? audio_codec->input_sample_rate() : 24000;
    const int input_ch   = audio_codec ? audio_codec->input_channels()    : 2;
    const bool input_ref = audio_codec ? audio_codec->input_reference()   : true;
    if (!initAfe(input_sr, input_ch, input_ref)) {
        mclog::tagError(_tag, "AFE init failed");
        deinitOpusCodec();
        volc_destroy(_engine);
        _engine  = nullptr;
        _running = false;
        return false;
    }

    startAfeFetch();
    startUplinkTask();
    startToolTask();
    startAudioBridge();

    // 会话期间关闭 WiFi 省电（IDF 默认 MIN_MODEM 的 DTIM 休眠会让 TX 周期性停顿
    // 数十至数百毫秒，RTC 上行音频流被随机掐断——服务端表现为"端侧停发/人声截断"）。
    // xiaozhi 链路在会话期也是 PERFORMANCE（application.cc SetPowerSaveLevel），
    // 这里对齐；stop() 时恢复 MIN_MODEM 省电。
    esp_wifi_set_ps(WIFI_PS_NONE);
    mclog::tagInfo(_tag, "wifi power save off for RTC session");

    // 视觉桥：低频 2s/帧，仅会话期取帧编码上传（任务内按 _conversation_active 门控）。
    // 早期禁用的内存顾虑已解除：V4L2 mmap 缓冲是板级常驻（与本任务无关），帧副本/
    // JPEG/任务栈全部走 PSRAM，对内部 SRAM 零增量；每帧日志可持续取证。
    startVideoBridge();

    // 连接完成，进入"等待唤醒"视觉态（中性表情 + 待机灯色）。
    _led_tool_override.store(false);
    {
        LvglLockGuard lock;
        // 舵机使能对齐 xiaozhi 链路（Hal::startXiaozhi 同款配置）：自动角度同步 +
        // 空闲自动释放力矩。volc 链路此前从未配置，是工具转头/摇头无实际动作的
        // 嫌疑配置差异。
        auto& motion = GetStackChan().motion();
        motion.setAutoAngleSyncEnabled(true);
        motion.setAutoTorqueReleaseEnabled(true);
        uiWaitingForWake();
    }
    mclog::tagInfo(_tag, "started");
    return true;
}

void stop()
{
    _conversation_connecting.store(false);
    stopAudioBridge();
    stopAfeFetch();
    stopUplinkTask();
    stopToolTask();
    stopVideoBridge();
    stopPlaybackTask();

    // Wait for capture/fetch/uplink/playback/tool tasks to drain before tearing
    // down AFE and the Opus codec so we don't free the iface/codec while another
    // task is still inside feed/fetch/encode/decode/startConversation. RTC 建联
    // 弱网下可达数秒，等满 15s 仍未退出时跳过资源释放，优于拆掉在用资源触发异常。
    bool tasks_exited = false;
    for (int i = 0; i < 750; ++i) {
        if (!_audio_task_started.load() && !_afe_fetch_started.load() &&
            !_uplink_task_started.load() && !_playback_task_started.load() &&
            !_tool_task_started.load() && !_connect_task_started.load() &&
            !_video_task_started.load()) {
            tasks_exited = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (tasks_exited) {
        deinitAfe();
        deinitOpusCodec();
    } else {
        mclog::tagError(_tag, "audio tasks still alive after 15s, skip AFE/Opus teardown");
    }

    // 锁序与发送方一致（_send_mutex → _mutex）：先等在途的 volc_send_* 完成，
    // 再销毁引擎，防止发送路径使用已 destroy 的引擎。
    std::lock_guard<std::mutex> send_lock(_send_mutex);
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_engine) {
        _running = false;
        return;
    }

    // 仅当 RTC 会话真正建联过(唤醒后 volc_start)才需要 volc_stop；
    // 待机未唤醒时 _conv_started=false，直接 destroy 即可。
    if (_conv_started.load()) {
        volc_stop(_engine);
    }
    volc_destroy(_engine);
    _engine  = nullptr;
    _running = false;
    _conv_started.store(false);
    _conversation_connecting.store(false);
    _conv_wakenet_disabled.store(false);
    _external_prompt_sent.store(false);
    _external_prompt_last_attempt_us.store(0);
    // 恢复 WiFi 省电（会话期间为保 RTC 实时性关闭）。
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    mclog::tagInfo(_tag, "stopped");
}

bool isRunning()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _running;
}

bool sendVideoJpeg(const uint8_t* data, size_t len)
{
    if (!data || len == 0) {
        return false;
    }

    // 视频帧较大（QVGA JPEG ~10-20KB），发送阻塞时间比音频帧更长：与音频发送同一
    // 锁纪律（_send_mutex 跨发送、_mutex 短校验），绝不持 _mutex 跨网络发送。
    // 仅会话建联后发送，待机期不上传视频。
    std::lock_guard<std::mutex> send_lock(_send_mutex);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_running || !_engine || !_conv_started.load()) {
            return false;
        }
    }

    volc_video_frame_info_t info = {};
    info.data_type = VOLC_VIDEO_DATA_TYPE_JPEG;
    return volc_send_video_data(_engine, data, len, &info) == 0;
}

bool interrupt()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // 待机未唤醒(会话未建联)时点击无对话可打断，直接忽略。
        if (!_running || !_engine || !_conv_started.load()) {
            mclog::tagInfo(_tag, "interrupt ignored: running={} engine={} conv_started={}",
                           _running, _engine != nullptr, _conv_started.load());
            return false;
        }
    }

    flushPlaybackQueue();
    if (_afe_iface && _afe_data) {
        _afe_iface->reset_buffer(_afe_data);
    }

    // 快照引擎指针即可：sendBinaryJsonMessage 内部会按锁序校验引擎存活，
    // 这里不持 _mutex 跨发送，避免阻塞发送时卡死 UI 线程。
    volc_engine_t engine = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_running || !_engine || !_conv_started.load()) {
            return false;
        }
        engine = _engine;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return false;
    }
    cJSON_AddStringToObject(root, "Command", "interrupt");
    const int ret = sendBinaryJsonMessage(engine, "ctrl", root);
    cJSON_Delete(root);
    if (ret != 0) {
        mclog::tagWarn(_tag, "interrupt send failed: {}", ret);
        return false;
    }
    mclog::tagInfo(_tag, "interrupt sent");
    return true;
}

}  // namespace volc_agent

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
static std::mutex _play_mutex;
static volc_engine_t _engine = nullptr;
static bool _running         = false;
// 引擎已 volc_start（RTC 会话已建联）。与 _running 区分：_running 表示 agent 任务
// 已起来（含 AFE 待机唤醒检测），_conv_started 表示真正与服务端建立了对话会话。
// 唤醒前 _running=true 但 _conv_started=false：只跑本地唤醒检测，不建联、不收发音频。
static std::atomic_bool _conv_started{false};
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
// 抖动预缓冲：服务端下行默认按 ~120ms 分包到达，且到达节奏不均。播放前先攒够
// _playback_prime_frames 个下行包再开始 drain，配合 I2S 60ms DMA 缓冲吸收抖动，
// 避免一来就播导致 DMA 欠载产生卡顿。仅在会话开始/flush 时预缓冲一次。
static std::atomic_bool _playback_priming{true};
constexpr size_t _playback_queue_max_frames = 32;
constexpr size_t _playback_prime_frames = 2;

// Async uplink queue: afeFetchTask only fetches + enqueues raw 16kHz PCM frames
// so it keeps draining AFE even when the network send blocks; a dedicated
// uplink task does the Opus encode + volc_send_audio_data. Decoupling prevents
// the AFE FEED ringbuffer from overflowing (and dropping mic data) whenever the
// RTC send stalls under network congestion.
static std::mutex _uplink_mutex;
static std::deque<std::vector<int16_t>> _uplink_queue;
static std::atomic_bool _uplink_task_running{false};
static std::atomic_bool _uplink_task_started{false};
constexpr size_t _uplink_queue_max_frames = 50;  // ~1s of 20ms frames

// AFE (Audio Front End) for local WakeNet. Replaces the previous half-duplex
// guard (`_assistant_speaking`) and software silence window. Pipeline:
//   codec input (24kHz / 2ch: mic + mic)
//     -> 24K->16K resampler
//     -> AFE feed (16kHz / "MM")
//     -> AFE fetch (16kHz / 1ch)
//     -> OPUS uplink
// Memory: AFE allocated with AFE_MEMORY_ALLOC_MORE_PSRAM; NS / AGC / WakeNet
// disabled to keep PSRAM/CPU/SRAM footprint minimal on CoreS3.
constexpr int _afe_sample_rate = 16000;
static const esp_afe_sr_iface_t* _afe_iface = nullptr;
static esp_afe_sr_data_t* _afe_data = nullptr;
static srmodel_list_t* _afe_models = nullptr;
static esp_ae_rate_cvt_handle_t _input_resampler  = nullptr; // 24K -> 16K
static afe_doa_handle_t* _doa_handle = nullptr;
static void* _opus_encoder = nullptr;
static void* _opus_decoder = nullptr;
static int _opus_encoder_input_bytes = 0;
static int _opus_encoder_output_bytes = 0;
// 下行解码直出采样率：设为 codec 输出率（CoreS3 为 24000），让 Opus 解码器内部
// 直接重采样到播放采样率，避免额外的最近邻软件重采样（音质差、引入卡顿听感）。
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
// 最近一次会话活动（唤醒/下行音频/明显上行人声）的时间戳，用于空闲超时回待机。
static std::atomic<int64_t> _last_activity_us{0};
static std::atomic_bool _external_prompt_sent{false};
static std::atomic<int64_t> _external_prompt_last_attempt_us{0};
constexpr int64_t _conv_idle_timeout_ms = 15000;  // 对话静默 15s 回等待唤醒
constexpr uint32_t _uplink_active_rms = 600;      // 上行帧 RMS 超过此值视为用户在讲话

// 会话 UI 状态切换（定义在文件后部），afeFetchTask 唤醒/超时时需要调用。
void uiWaitingForWake();
void uiListening();
void uiSpeaking();
void sendExternalDatePromptIfNeeded();
void startPlaybackTask();
void startUplinkTask();
void stopUplinkTask();

// 会话建联/断联（定义在文件后部）。唤醒后才 volc_start 建联，空闲超时后 volc_stop
// 断联回待机，避免一进入链路服务端就主动推欢迎语开始对话。
bool startConversation();
void stopConversation();

constexpr int _volc_audio_sample_rate = 16000;
constexpr int _opus_uplink_frame_ms = 20;
constexpr int _opus_downlink_frame_ms = 20;
constexpr size_t _opus_uplink_frame_samples = _volc_audio_sample_rate * _opus_uplink_frame_ms / 1000;
constexpr int _opus_bitrate = 24000;
constexpr size_t _doa_window_frames = 1024;
constexpr int _video_frame_interval_ms = 3000;

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
      "{\"audio\":{\"codec\":{\"opus\":{\"sample_rate\":16000,\"channels\":1,\"s_samples_per_frame\":320}}}}"
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
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
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
    // 解码直出 codec 播放采样率：Opus 内部固定 48kHz 运行，可直接重采样到任意输出率。
    // 设为播放采样率后省去后续软件重采样（最近邻），上行仍按 16kHz 编码不受影响。
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
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_running || !_engine || !data || len == 0) {
        return false;
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
    }
    _uplink_queue.emplace_back(pcm, pcm + samples);
}

void uplinkTask(void*)
{
    std::vector<uint8_t> encoded;
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
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (encodePcmToOpus(pcm.data(), pcm.size(), encoded)) {
            sendAudioOpus(encoded.data(), encoded.size());
        }
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
    // 任务栈放 PSRAM，原因同 startAudioBridge。
    // 固定到 core 0：core 1 已跑 volc_audio(AFE/WakeNet 计算密集) + volc_play + SDK
    // 的 VolcRTCMain，再叠加 uplink 会饿死 IDLE1；core 0 仅 fetch(只入队，很轻)，
    // 把编码放这里达成 core0=fetch+uplink / core1=capture+play 的 2/2 均衡。
    if (xTaskCreatePinnedToCoreWithCaps(uplinkTask, "volc_uplink", 8192, nullptr, 5, nullptr, 0,
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
            vTaskDelay(pdMS_TO_TICKS(10));
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
    if (xTaskCreatePinnedToCoreWithCaps(playbackTask, "volc_play", 32768, nullptr, 5, nullptr, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        _playback_task_running.store(false);
        _playback_task_started.store(false);
        mclog::tagError(_tag, "failed to create playback task");
    }
}

void stopPlaybackTask()
{
    _playback_task_running.store(false);
    flushPlaybackQueue();
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

    if (mic_count >= 2) {
        _doa_handle = afe_doa_create(input_format.c_str(), _afe_sample_rate, 20.0f, 0.06f, 1024);
        if (!_doa_handle) {
            mclog::tagWarn(_tag, "DOA init failed");
        }
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

    std::vector<int16_t> input_buf(src_frames_per_chunk * channels);
    std::vector<int16_t> resampled(resample_max_out_per_ch * channels);
    std::vector<int16_t> doa_window;
    doa_window.reserve(_doa_window_frames * channels);
    uint32_t feed_count = 0;

    while (_audio_task_running.load()) {
        if (!audio_codec->InputData(input_buf)) {
            vTaskDelay(pdMS_TO_TICKS(5));
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
            if (channels >= 2) {
                for (size_t i = 0; i < probe; ++i) {
                    int32_t m = input_buf[i * channels + 0];
                    int32_t r = input_buf[i * channels + channels - 1];
                    mic_sq += static_cast<uint64_t>(m * m);
                    ref_sq += static_cast<uint64_t>(r * r);
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
            mclog::tagInfo(_tag, "afe feed frames={} chunk={} mic_rms={} ref_rms={}",
                           feed_count, feed_chunk, mic_rms, ref_rms);
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

    std::vector<int16_t> uplink_accum;
    uplink_accum.reserve(_opus_uplink_frame_samples * 2);
    uint32_t uplink_frames = 0;

    while (_afe_fetch_running.load()) {
        afe_fetch_result_t* res =
            _afe_iface->fetch_with_delay(_afe_data, pdMS_TO_TICKS(100));
        if (!_afe_fetch_running.load()) {
            break;
        }
        if (!res || res->ret_value == ESP_FAIL || !res->data || res->data_size <= 0) {
            continue;
        }

        const int64_t now_us = esp_timer_get_time();

        // 唤醒门控：检测到 "Hi Stack-Chan" 时建联并进入对话；丢弃唤醒词残留缓冲，
        // 避免把唤醒词本身当作首句话上行。
        if (res->wakeup_state == WAKENET_DETECTED) {
            if (!_conversation_active.load()) {
                mclog::tagInfo(_tag, "wake word detected, starting conversation");
                turnToWakeSource(res);
                Board::GetInstance().GetDisplay()->SetChatMessage("system", "连接中");
                if (!startConversation()) {
                    mclog::tagWarn(_tag, "startConversation failed, stay in wake-wait");
                    Board::GetInstance().GetDisplay()->SetChatMessage("system", "待连接");
                    continue;
                }
                _conversation_active.store(true);
                // 会话期关闭 WakeNet：对话中不再靠唤醒词进入（退出由 idle timeout +
                // 服务端打断驱动），关掉每帧的唤醒词 CNN 推理给 core 1 显著减负。
                if (_afe_iface->disable_wakenet) {
                    _afe_iface->disable_wakenet(_afe_data);
                }
                _last_activity_us.store(now_us);
                uplink_accum.clear();
                Board::GetInstance().GetDisplay()->SetChatMessage("system", "连接成功");
                uiListening();
            }
            continue;
        }

        // 未唤醒：保持喂 WakeNet 检测，但不上行任何音频。
        if (!_conversation_active.load()) {
            continue;
        }

        sendExternalDatePromptIfNeeded();

        // 对话中：长时间无任何活动(无下行、无明显人声)则断联回到等待唤醒。
        if ((now_us - _last_activity_us.load()) > _conv_idle_timeout_ms * 1000) {
            mclog::tagInfo(_tag, "conversation idle timeout, back to wake-wait");
            _conversation_active.store(false);
            stopConversation();
            // 回待机：重新开启 WakeNet 并清空 ringbuf，丢弃会话期残留音频避免误唤醒。
            if (_afe_iface->enable_wakenet) {
                _afe_iface->enable_wakenet(_afe_data);
            }
            if (_afe_iface->reset_buffer) {
                _afe_iface->reset_buffer(_afe_data);
            }
            uiWaitingForWake();
            Board::GetInstance().GetDisplay()->SetChatMessage("system", "待连接");
            uplink_accum.clear();
            continue;
        }

        const size_t in_samples = res->data_size / sizeof(int16_t);
        uplink_accum.insert(uplink_accum.end(), res->data, res->data + in_samples);

        while (uplink_accum.size() >= _opus_uplink_frame_samples) {
            // 只把 PCM 帧入队，编码+网络发送交给 volc_uplink 任务。fetch 循环保持轻量，
            // 即使网络发送阻塞也不会卡住 fetch()，从而避免 AFE FEED ringbuffer 溢出丢麦克风数据。
            enqueueUplink(uplink_accum.data(), _opus_uplink_frame_samples);

            ++uplink_frames;

            // 上行能量：用于（a）日志监控（b）检测用户是否在讲话以刷新空闲计时。
            uint64_t out_sq = 0;
            const size_t probe = std::min<size_t>(_opus_uplink_frame_samples, 256);
            for (size_t i = 0; i < probe; ++i) {
                int32_t v = uplink_accum[i];
                out_sq += static_cast<uint64_t>(v * v);
            }
            uint32_t out_rms = probe ? static_cast<uint32_t>(
                                  std::sqrt(static_cast<double>(out_sq) / probe)) : 0;
            if (out_rms >= _uplink_active_rms) {
                _last_activity_us.store(esp_timer_get_time());
            }
            if (uplink_frames == 1 || uplink_frames % 250 == 0) {
                mclog::tagInfo(_tag, "afe uplink frames={} vad={} samples={} out_rms={}",
                               uplink_frames, static_cast<int>(res->vad_state),
                               static_cast<int>(_opus_uplink_frame_samples),
                               out_rms);
            }

            uplink_accum.erase(uplink_accum.begin(),
                               uplink_accum.begin() + _opus_uplink_frame_samples);
        }
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
    while (_video_task_running.load()) {
        auto camera = hal_bridge::board_get_camera();
        if (!camera) {
            vTaskDelay(pdMS_TO_TICKS(_video_frame_interval_ms));
            continue;
        }

        if (camera->StreamCaptures()) {
            const uint8_t* frame_data = camera->GetFrameData();
            size_t frame_size         = camera->GetFrameSize();
            int width                 = camera->GetFrameWidth();
            int height                = camera->GetFrameHeight();
            int format                = camera->GetFrameFormat();

            uint8_t* jpeg_data = nullptr;
            size_t jpeg_len    = 0;
            if (frame_data && frame_size > 0 &&
                image_to_jpeg((uint8_t*)frame_data, frame_size, width, height, (v4l2_pix_fmt_t)format, 20,
                              &jpeg_data, &jpeg_len)) {
                if (jpeg_data) {
                    volc_agent::sendVideoJpeg(jpeg_data, jpeg_len);
                    free(jpeg_data);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(_video_frame_interval_ms));
    }

    _video_task_started.store(false);
    vTaskDelete(nullptr);
}

void startVideoBridge()
{
    bool expected = false;
    if (!_video_task_started.compare_exchange_strong(expected, true)) {
        return;
    }

    _video_task_running.store(true);
    if (xTaskCreatePinnedToCore(videoCaptureTask, "volc_video", 6144, nullptr, 3, nullptr, 1) != pdPASS) {
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
// 注意：SetStatus 内部会按字符串自行设灯（LISTENING=蓝、SPEAKING=蓝），所以
// 这里在 SetStatus 之后再 showRgbColor 覆盖成期望颜色作为唯一真相源。

void uiWaitingForWake()
{
    // 待机等待唤醒：中性表情 + 柔和绿灯，移除说话口型动画。
    auto* d = Board::GetInstance().GetDisplay();
    d->SetStatus(Lang::Strings::LISTENING);  // 复用其"移除 speaking 动画"的逻辑
    d->SetEmotion("neutral");
    GetHAL().showRgbColor(0x00, 0x30, 0x10);
}

void uiListening()
{
    // 已唤醒、等待/聆听用户：中性表情 + 绿灯。
    auto* d = Board::GetInstance().GetDisplay();
    d->SetStatus(Lang::Strings::LISTENING);
    d->SetEmotion("neutral");
    GetHAL().showRgbColor(0x00, 0x40, 0x00);
}

void uiSpeaking()
{
    // 助手说话：开心表情 + 口型动画 + 蓝灯。
    auto* d = Board::GetInstance().GetDisplay();
    d->SetStatus(Lang::Strings::SPEAKING);  // 添加 SpeakingModifier 口型动画
    d->SetEmotion("happy");
    GetHAL().showRgbColor(0x00, 0x00, 0x40);
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
    const int ret = volc_send_message(engine, message.data(), message.size(), &info);
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
    sendExternalDatePromptIfNeeded();
    switch (status) {
        case VOLC_CONV_STATUS_LISTENING:
            mclog::tagInfo(_tag, "listening");
            uiListening();
            break;
        case VOLC_CONV_STATUS_THINKING:
            mclog::tagInfo(_tag, "thinking");
            Board::GetInstance().GetDisplay()->SetEmotion("doubtful");
            GetHAL().showRgbColor(0x40, 0x30, 0x00);
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

        LvglLockGuard lock;
        GetStackChan().leftNeonLight().setColor(red, green, blue);
        GetStackChan().rightNeonLight().setColor(red, green, blue);
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
    }

    cJSON_Delete(args);
    return result;
}

void sendToolResult(volc_engine_t engine, const char* call_id, const std::string& content)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ToolCallID", call_id ? call_id : "");
    cJSON_AddStringToObject(root, "Content", content.c_str());

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
        const char* args_json = cJSON_GetStringValue(arguments);
        if (!call_id || !tool_name) {
            continue;
        }

        mclog::tagInfo(_tag, "tool call: {}", tool_name);
        sendToolResult(engine, call_id, dispatchTool(tool_name, args_json));
    }
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

    std::string subtitle;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, data) {
        cJSON* text = cJSON_GetObjectItem(item, "text");
        const char* value = cJSON_GetStringValue(text);
        if (value && value[0]) {
            subtitle += value;
        }
    }
    if (!subtitle.empty()) {
        Board::GetInstance().GetDisplay()->SetChatMessage("assistant", subtitle.c_str());
    }
}

void onMessageData(volc_engine_t, const void* data, size_t len, volc_message_info_t*, void*)
{
    if (!data || len == 0) {
        return;
    }

    mclog::tagInfo(_tag, "message received: {} bytes", len);
    const char* text = static_cast<const char*>(data);
    if (len > 8 && (std::memcmp(text, "tool", 4) == 0 || std::memcmp(text, "subv", 4) == 0)) {
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
        cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
        if (!root) {
            mclog::tagWarn(_tag, "message json parse failed");
            return;
        }

        if (std::memcmp(text, "tool", 4) == 0) {
            handleToolMessage(_engine, root);
        } else {
            handleSubtitleMessage(root);
        }
        cJSON_Delete(root);
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
    std::lock_guard<std::mutex> lock(_mutex);
    if (_running) {
        return true;
    }

    if (!hasRequiredConfig()) {
        mclog::tagError(_tag, "missing volc config");
        return false;
    }

    if (!waitForSystemTime()) {
        mclog::tagWarn(_tag, "system time not synced before volc_start");
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
    _conv_started.store(false);
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
    startAudioBridge();
    // 视频桥暂时禁用：摄像头的 mmap buffer (~150KB) + JPEG 编码会进一步压低内部 SRAM，
    // 当前 AEC + AFE 已经把可用 SRAM 压到 5KB 以下，先把视频路径让给音频确保链路活下来。
    // 连接完成，进入"等待唤醒"视觉态（中性表情 + 待机灯色）。
    {
        LvglLockGuard lock;
        uiWaitingForWake();
    }
    mclog::tagInfo(_tag, "started");
    return true;
}

void stop()
{
    stopAudioBridge();
    stopAfeFetch();
    stopUplinkTask();
    stopVideoBridge();
    stopPlaybackTask();

    // Wait for capture/fetch/uplink tasks to drain before tearing down AFE and
    // the Opus codec so we don't free the iface/codec while another task is
    // still inside feed/fetch/encode/decode.
    for (int i = 0; i < 50; ++i) {
        if (!_audio_task_started.load() && !_afe_fetch_started.load() &&
            !_uplink_task_started.load() && !_playback_task_started.load()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    deinitAfe();
    deinitOpusCodec();

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
    _external_prompt_sent.store(false);
    _external_prompt_last_attempt_us.store(0);
    mclog::tagInfo(_tag, "stopped");
}

bool isRunning()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _running;
}

bool sendVideoJpeg(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_running || !_engine || !data || len == 0) {
        return false;
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
            return false;
        }
    }

    flushPlaybackQueue();
    if (_afe_iface && _afe_data) {
        _afe_iface->reset_buffer(_afe_data);
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_running || !_engine || !_conv_started.load()) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return false;
    }
    cJSON_AddStringToObject(root, "Command", "interrupt");
    const int ret = sendBinaryJsonMessage(_engine, "ctrl", root);
    cJSON_Delete(root);
    if (ret != 0) {
        mclog::tagWarn(_tag, "interrupt send failed: {}", ret);
        return false;
    }
    return true;
}

}  // namespace volc_agent

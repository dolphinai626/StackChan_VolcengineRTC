Status: [RESOLVED] CoreS3 硬件支持 AEC（有硬件回采），当前固件未启用

# Debug Session: aec-hardware-capability

## Symptoms
- 需要确认 StackChan(CoreS3) 硬件是否支持 AEC（声学回声消除）。
- 当前 Volc 链路用半双工门控规避回声，想评估能否上真正的全双工 AEC。

## Hypotheses
1. CoreS3 像低端板一样只有麦克风、没有功放回采 → 不支持硬件 AEC（早期结论，已被推翻）。
2. CoreS3 的 ES7210 是 4 通道 ADC，可能有一路接了功放回采参考 → 支持硬件 AEC。

## Evidence
- 官方原理图 Sch_M5_CoreS3_v1.0.pdf 音频页（用户实地核对）确认：
  - ES7210(U9, I2C 0x40) 4 通道 ADC：MIC1(pin15/16)、MIC2(pin19/20) 接双麦；
    **MIC3(pin31/32) 接 AEC_P/AEC_N**。
  - AEC_P/AEC_N 来自 AW88298(U8) 的喇叭差分输出 SPK_VOP/SPK_VON，
    经 R40/R42(150K/1%, 已贴) + C103/C105(1uF) 耦合 + 22pF 滤波接入。
  - 网络名**直接命名为 AEC_P / AEC_N**，是 M5 官方为回声消除设计的硬件回采。
- 对比 ESP-BOX-3(xiaozhi 官方支持 device AEC)：用满 ES7210 四通道；
  而 CoreS3 当前固件 `cores3_audio_codec.cc` 只选 `MIC1 | MIC2`（2 路），
  `config.h` 里 `AUDIO_INPUT_REFERENCE=false`，**没有启用 MIC3 回采**。
- 早期 debug-wakeup-source-localization.md 里"开 3 通道 esp_codec_dev_read 返回
  ESP_FAIL / Not support channel 3 / RMS=[0,0,0]"——重新定性为 **TDM/驱动配置问题**
  （3 通道 slot mask 与 esp_codec_dev channel 数没配对），**不是硬件缺失**。

## Conclusion
- **CoreS3 硬件支持 AEC**：ES7210 MIC3 通道接了 SPK 差分回采（网络名 AEC_P/AEC_N），
  硬件回采参考是齐全的。
- 缺的是**固件启用**：当前只读 2 麦、没读 MIC3 回采、AFE 没开 AEC。
- 早期"CoreS3 无硬件回采、AEC 不可用"的结论是**错的**（AGENTS.md §8 已据此更正）。

## Implementation Path（软件 AEC 方案，待 AEC 分支验证）
1. ES7210：`mic_selected` 加 MIC3 → `MIC1 | MIC2 | MIC3`（2 麦 + 1 回采）。
2. I2S RX TDM：配 3（或 4）通道 slot，对齐 ES7210 TDM 输出——这正是早期
   "Not support channel 3" 卡住处，需 slot mask / esp_codec_dev `channel` 配对。
3. codec：`input_channels=3`、`AUDIO_INPUT_REFERENCE=true`。
4. AFE：用 `MMR` 格式喂入（M=MIC1、M=MIC2、R=MIC3 回采），打开 AFE AEC 模块。
5. 注意 AW88298 是 D 类功放（PWM 输出），R40/R42(150K)+耦合网络已把回采滤成可用参考；
   AFE AEC 自适应滤波能用此参考收敛。
6. **隔离风险**：`cores3_audio_codec.cc` 是 board 级、两条链路共用。改 3 通道 TDM 会
   影响 AI.Agent(xiaozhi) 链路。本次只验证 volc 链路，需让 reference 模式可选
   （volc 开、xiaozhi 不开），避免污染原生链路。

## Verification
- 待 AEC 分支实测：开 MIC3 回采 + AFE AEC 后，TTS 播放期间上行不再含回声、
  可做语音打断（替掉半双工门控）。验证通过才考虑合并 main。

# StackChan Local Changes

Base upstream: `78/xiaozhi-esp32` tag `v2.2.4` / commit `e77dedb`.

This directory is vendored into `StackChan_VolcengineRTC` instead of being kept
as a Git submodule, so the StackChan-specific patches are preserved directly in
this repository.

## Differences From Upstream

- `main/application.cc`: disables activation-code audio playback and shows a
  simple binding/setup message on the display instead, reducing SRAM pressure
  during activation.
- `main/assets.cc` and `main/assets.h`: disable the upstream Emote asset
  strategy path so StackChan can use the LVGL asset strategy without depending
  on the upstream Emote display runtime.
- `main/audio/wake_words/afe_wake_word.cc` and
  `main/audio/wake_words/afe_wake_word.h`: allocate the AFE audio detection task
  stack in PSRAM and keep the static task buffer in internal RAM, avoiding task
  creation failure when internal SRAM is fragmented.
- `main/boards/common/i2c_device.cc` and `main/boards/common/i2c_device.h`:
  replace fatal `ESP_ERROR_CHECK` I2C accessors with warning logs and add
  `TryReadRegs()` for callers that need non-fatal probing.

The effective patch relative to upstream is:

```bash
git diff e77dedb..7c4c8d5 -- main/application.cc main/assets.cc main/assets.h \
  main/audio/wake_words/afe_wake_word.cc main/audio/wake_words/afe_wake_word.h \
  main/boards/common/i2c_device.cc main/boards/common/i2c_device.h
```

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace volc_agent {

bool start();
void stop();
bool isRunning();
bool sendVideoJpeg(const uint8_t* data, size_t len);
bool interrupt();
// 提醒触发时通知云端 LLM 用 TTS 播报（仅会话建联时生效，返回是否已发送）。
bool notifyReminder(std::string_view message);

}  // namespace volc_agent

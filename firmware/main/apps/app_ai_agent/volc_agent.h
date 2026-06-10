/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace volc_agent {

bool start();
void stop();
bool isRunning();
bool sendVideoJpeg(const uint8_t* data, size_t len);
bool interrupt();

}  // namespace volc_agent

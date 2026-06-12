/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <cstdint>

/**
 * @brief Derived App
 *
 */
class AppAiAgent : public mooncake::AppAbility {
public:
    enum class LaunchMode {
        StackChan,
        Volcengine,
    };

    explicit AppAiAgent(LaunchMode mode = LaunchMode::StackChan);

    // Override lifecycle callbacks
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class Backend {
        None,
        Xiaozhi,
        Volcengine,
    };

    LaunchMode _mode;
    Backend _active_backend        = Backend::None;
    Backend _pending_backend       = Backend::None;
    bool _volc_started             = false;
    bool _volc_starting            = false;
    bool _volc_start_failed        = false;
    bool _volc_attempted           = false;
    bool _launch_xiaozhi_on_close  = false;

    void startVolcengineBackend();
};

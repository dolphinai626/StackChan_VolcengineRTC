/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <apps/app_setup/view/view.h>
#include <mooncake.h>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * @brief Derived App
 *
 */
class AppAiAgent : public mooncake::AppAbility {
public:
    AppAiAgent();

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

    std::vector<view::SelectMenuPage::MenuSection> _menu_sections;
    std::unique_ptr<view::SelectMenuPage> _menu_page;
    Backend _active_backend        = Backend::None;
    Backend _pending_backend       = Backend::None;
    bool _volc_started             = false;
    bool _volc_starting            = false;
    bool _volc_start_failed        = false;
    bool _volc_attempted           = false;
    bool _launch_xiaozhi_on_close  = false;

    void startVolcengineBackend();
};

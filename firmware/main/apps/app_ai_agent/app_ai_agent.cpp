/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_ai_agent.h"
#include "volc_agent.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <assets/lang_config.h>
#include <board.h>
#include <display/display.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

AppAiAgent::AppAiAgent()
{
    // Configure App name
    setAppInfo().name = "AI.AGENT";
    // Configure App icon
    static auto icon  = assets::get_image("icon_ai_agent.bin");
    setAppInfo().icon = (void*)&icon;
    // Configure App theme color
    static uint32_t theme_color = 0x33CC99;
    setAppInfo().userData       = (void*)&theme_color;
}

// Called when the App is installed
void AppAiAgent::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

// Called when the App is opened
// You can construct UI, initialize operations, etc. here
void AppAiAgent::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _active_backend = Backend::None;
    _pending_backend = Backend::None;
    _volc_started = false;
    _volc_starting = false;
    _volc_start_failed = false;
    _launch_xiaozhi_on_close = false;

    _menu_sections = {{
        "AI.Agent",
        {
            {"StackChan",
             [&]() {
                 mclog::tagInfo(getAppInfo().name, "StackChan selected");
                 _pending_backend = Backend::Xiaozhi;
             }},
            {"Volcengine",
             [&]() {
                 mclog::tagInfo(getAppInfo().name, "Volcengine selected");
                 _pending_backend = Backend::Volcengine;
             }},
        },
    }};

    LvglLockGuard lock;
    _menu_page = std::make_unique<view::SelectMenuPage>(_menu_sections);
    view::create_home_indicator([&]() { close(); });
    view::create_status_bar();
}

// Called repeatedly while the App is running
void AppAiAgent::onRunning()
{
    Backend pending_backend = Backend::None;

    {
        LvglLockGuard lock;

        if (_menu_page) {
            _menu_page->update();
        }

        pending_backend = _pending_backend;
        _pending_backend = Backend::None;
        if (pending_backend != Backend::None) {
            _menu_page.reset();
        }

        if (_volc_start_failed) {
            _volc_start_failed = false;
            _active_backend = Backend::None;
            _menu_page = std::make_unique<view::SelectMenuPage>(_menu_sections);
            view::create_home_indicator([&]() { close(); });
        }

        GetStackChan().update();
        view::update_home_indicator();
        view::update_status_bar();
    }

    if (pending_backend == Backend::Xiaozhi) {
        _active_backend = Backend::Xiaozhi;
        _launch_xiaozhi_on_close = true;
        close();
    } else if (pending_backend == Backend::Volcengine) {
        _active_backend = Backend::Volcengine;
        _volc_starting = true;
        _volc_attempted = true;
        {
            LvglLockGuard lock;
            // 进入即创建并显示 avatar（SetupUI 幂等），避免连接期间中部黑屏；
            // 显示待机表情 + "待连接" 状态，唤醒后才开始建联。
            Board::GetInstance().GetDisplay()->SetupUI();
            Board::GetInstance().GetDisplay()->SetEmotion("neutral");
            Board::GetInstance().GetDisplay()->SetChatMessage("system", "待连接");
            view::create_home_indicator([&]() { close(); }, 0x81DBBD, 0x134233);
            view::create_status_bar(0x81DBBD, 0x134233);
        }
        if (xTaskCreate([](void* arg) {
                static_cast<AppAiAgent*>(arg)->startVolcengineBackend();
                vTaskDelete(nullptr);
            }, "volc_start", 8192, this, 3, nullptr) != pdPASS) {
            mclog::tagError(getAppInfo().name, "failed to create volc start task");
            _volc_starting = false;
            close();
        }
    }
}

// Called when the App is closed
// You can destroy UI, release resources, etc. here
void AppAiAgent::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    {
        LvglLockGuard lock;
        _menu_page.reset();
        view::destroy_home_indicator();
    }

    if (_volc_started) {
        volc_agent::stop();
        _volc_started = false;
    }

    if (_launch_xiaozhi_on_close) {
        _launch_xiaozhi_on_close = false;
        // 已尝试过 Volcengine 时，HAL 网络栈和 SNTP 已初始化，xiaozhi 内部 StartNetwork
        // 会发现 station 已 active 并卡在等待 Connected 事件直至超时进入配网模式。
        // 通过 reboot 让设备从干净状态重新进入 xiaozhi，保持 StackChan 链路不被污染。
        if (_volc_attempted) {
            mclog::tagInfo(getAppInfo().name, "volc was attempted, reboot to start xiaozhi cleanly");
            GetHAL().reboot();
            return;
        }
        GetHAL().requestXiaozhiStart();
    }

    _active_backend = Backend::None;
}

void AppAiAgent::startVolcengineBackend()
{
    GetHAL().startNetwork([&](std::string_view msg) {
        mclog::tagInfo(getAppInfo().name, "{}", msg);
    });

    _volc_started = volc_agent::start();
    _volc_starting = false;
    if (!_volc_started) {
        mclog::tagError(getAppInfo().name, "failed to start volc agent");
        _volc_start_failed = true;
        return;
    }

    // Reuse StackChanAvatarDisplay to render Volc conversation (avatar, status, chat).
    // SetupUI is idempotent (guarded by setup_ui_called_) so it is safe across re-entries.
    // 连接完成后的"等待唤醒"视觉态已由 volc_agent::start() 内部统一设置，这里不再重复
    // SetStatus，避免覆盖唤醒门控的灯色/表情。
    {
        LvglLockGuard lock;
        Board::GetInstance().GetDisplay()->SetupUI();
    }
}

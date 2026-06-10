/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <stackchan/stackchan.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <wifi_manager.h>
#include <board.h>
#include <mutex>
#include <queue>
#include <vector>
#include <ctime>
#include <sys/time.h>
#include <esp_sntp.h>
#include <atomic>

static std::string _tag           = "Network";
static bool _is_network_connected = false;

static void time_sync_notification_cb(struct timeval* tv)
{
    mclog::tagInfo(_tag, "SNTP time synchronized");
    GetHAL().syncSystemTimeToRtc();
}

void Hal::startSntp()
{
    mclog::tagInfo(_tag, "SNTP init");

    if (esp_sntp_enabled()) {
    } else {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_setservername(2, "cn.pool.ntp.org");

        sntp_set_time_sync_notification_cb(time_sync_notification_cb);

        esp_sntp_init();
    }
}

void Hal::startNetwork(std::function<void(std::string_view)> onLog)
{
    if (_is_network_connected) {
        mclog::tagInfo(_tag, "network already connected");
        return;
    }

    std::atomic<bool> network_connected = false;
    std::atomic<int> pending_event      = -1;

    auto& board = Board::GetInstance();
    mclog::tagInfo(_tag, "start and wait for network connected...");

    board.SetNetworkEventCallback([&network_connected, &pending_event](NetworkEvent event, const std::string&) {
        switch (event) {
            case NetworkEvent::Connected: {
                network_connected = true;
                break;
            }
            case NetworkEvent::Scanning:
            case NetworkEvent::Connecting:
            case NetworkEvent::WifiConfigModeEnter:
                pending_event = static_cast<int>(event);
                break;
            case NetworkEvent::Disconnected:
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                break;
            case NetworkEvent::ModemErrorNoSim:
                break;
            case NetworkEvent::ModemErrorRegDenied:
                break;
            case NetworkEvent::ModemErrorInitFailed:
                break;
            case NetworkEvent::ModemErrorTimeout:
                break;
        }
    });
    board.StartNetwork();

    int handled_event = -1;
    while (!network_connected) {
        int event = pending_event.exchange(-1);
        if (event >= 0 && event != handled_event) {
            handled_event = event;
            switch (static_cast<NetworkEvent>(event)) {
                case NetworkEvent::Scanning:
                    if (onLog) {
                        onLog("WiFi scanning...");
                    }
                    break;
                case NetworkEvent::Connecting:
                    if (onLog) {
                        onLog("WiFi connecting...");
                    }
                    break;
                case NetworkEvent::WifiConfigModeEnter: {
                    auto& wifi_manager = WifiManager::GetInstance();
                    auto msg = fmt::format("Enter WiFi config mode. Hotspot: {}, Config URL: {}",
                                           wifi_manager.GetApSsid(), wifi_manager.GetApWebUrl());
                    if (onLog) {
                        onLog(msg);
                    }
                    break;
                }
                default:
                    break;
            }
        }
        GetHAL().delay(500);
    }
    mclog::tagInfo(_tag, "network connected");
    board.SetNetworkEventCallback(nullptr);

    startSntp();

    _is_network_connected = true;
}

WifiStatus Hal::getWifiStatus()
{
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return WifiStatus::None;
    }
    if (!wifi.IsConnected()) {
        return WifiStatus::None;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return WifiStatus::High;
    } else if (rssi >= -75) {
        return WifiStatus::Medium;
    }
    return WifiStatus::Low;
}

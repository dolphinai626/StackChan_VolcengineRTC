/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <mooncake_log.h>
#include <mcp_server.h>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <board.h>
#include <display/display.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

using namespace stackchan;

static const std::string_view _tag = "HAL-MCP";

// 工具调用过程上字幕（与 volc 链路 handleToolMessage 的显示逻辑一致）。
// 只用全字体确定包含的常用字符，勿用特殊符号（字体缺字形会静默不渲染）。
static void _show_tool_subtitle(const char* tool_name, const char* state)
{
    const char* short_name =
        std::strncmp(tool_name, "self.robot.", 11) == 0 ? tool_name + 11 : tool_name;
    auto* display = Board::GetInstance().GetDisplay();
    if (display) {
        display->SetChatMessage("system", fmt::format("[工具] {} {}", short_name, state).c_str());
    }
}

void Hal::xiaozhi_mcp_init()
{
    mclog::tagInfo(_tag, "init");

    // https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-usage.md
    auto& mcp_server = McpServer::GetInstance();

    // System Prompt：
    // You can control the robot's head. Use get_yaw and get_pitch to sense current position. Use set_yaw for horizontal
    // movement and set_pitch for vertical movement. All angles are in degrees.

    mclog::tagInfo(_tag, "add robot.get_head_angles tool");
    mcp_server.AddTool("self.robot.get_head_angles",
                       "Returns current yaw/pitch in degrees. Neutral position is {yaw:0, pitch:0}.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           _show_tool_subtitle("get_head_angles", "查询中");
                           LvglLockGuard lock;  // StackChan motion update is under the lvgl lock

                           auto& motion      = GetStackChan().motion();
                           int current_yaw   = motion.yawServo().getCurrentAngle() / 10;
                           int current_pitch = motion.pitchServo().getCurrentAngle() / 10;

                           auto result = fmt::format(R"({{"yaw": {}, "pitch": {}}})", current_yaw, current_pitch);
                           mclog::tagInfo(_tag, "get_head_angles: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add robot.set_head_angles tool");
    mcp_server.AddTool("self.robot.set_head_angles",
                       "Adjust head position. GUIDELINES: "
                       "1. For natural interaction, stay within +/- 45 degrees. "
                       "2. Only use values > 70 if the user explicitly asks to look far away/behind. "
                       "3. Max ranges: Yaw(-128 to 128, -128 as your left), Pitch(0 to 90, 90 as your up). "
                       "Speed(100-1000, 150 is natural).",
                       PropertyList({Property("yaw", kPropertyTypeInteger, -9999, -9999, 128),
                                     Property("pitch", kPropertyTypeInteger, -9999, -9999, 90),
                                     Property("speed", kPropertyTypeInteger, 150, 100, 1000)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int speed = properties["speed"].value<int>();
                           int yaw   = properties["yaw"].value<int>();
                           int pitch = properties["pitch"].value<int>();

                           mclog::tagInfo(_tag, "motion set_angles: yaw: {}, pitch: {}, speed: {}", yaw, pitch, speed);
                           _show_tool_subtitle("set_head_angles", "执行中");

                           LvglLockGuard lock;

                           auto& motion = GetStackChan().motion();
                           if (pitch != -9999) {
                               motion.pitchServo().moveWithSpeed(pitch * 10, speed);
                           }
                           if (yaw != -9999) {
                               motion.yawServo().moveWithSpeed(yaw * 10, speed);
                           }

                           return true;
                       });

    // 与 volc 链路 dispatchTool 的 self.robot.shake_head 能力对齐。注意：xiaozhi 的
    // MCP 工具回调经 app.Schedule 在主循环执行，阻塞时长须收敛（times/pause 上限
    // 比 volc 链路更保守），避免卡住 xiaozhi 应用循环。
    mclog::tagInfo(_tag, "add robot.shake_head tool");
    mcp_server.AddTool("self.robot.shake_head",
                       "Shake head left-right to express denial or playfulness. "
                       "Blocks for up to ~1.5s, use sparingly.",
                       PropertyList({Property("times", kPropertyTypeInteger, 2, 1, 3),
                                     Property("amplitude", kPropertyTypeInteger, 25, 5, 60),
                                     Property("speed", kPropertyTypeInteger, 250, 100, 1000),
                                     Property("pause_ms", kPropertyTypeInteger, 180, 50, 300)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           const int times     = properties["times"].value<int>();
                           const int amplitude = properties["amplitude"].value<int>();
                           const int speed     = properties["speed"].value<int>();
                           const int pause_ms  = properties["pause_ms"].value<int>();

                           mclog::tagInfo(_tag, "shake_head: times={} amplitude={} speed={} pause={}",
                                          times, amplitude, speed, pause_ms);
                           _show_tool_subtitle("shake_head", "执行中");

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

                           return true;
                       });

    mclog::tagInfo(_tag, "add robot.set_led_color tool");
    mcp_server.AddTool(
        "self.robot.set_led_color",
        "Set the color of the robot's INTERNAL onboard LED. This is NOT for room lights. "
        "Values: 0-168 (safe range). Red=168,0,0; Green=0,168,0; Blue=0,0,168; White=100,100,100; Off=0,0,0.",
        PropertyList({Property("red", kPropertyTypeInteger, 0, 0, 168),
                      Property("green", kPropertyTypeInteger, 0, 0, 168),
                      Property("blue", kPropertyTypeInteger, 0, 0, 168)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int r = properties["red"].value<int>();
            int g = properties["green"].value<int>();
            int b = properties["blue"].value<int>();

            mclog::tagInfo(_tag, "set_led_color: r={}, g={}, b={}", r, g, b);
            _show_tool_subtitle("set_led_color", "执行中");

            LvglLockGuard lock;

            GetStackChan().leftNeonLight().setColor(r, g, b);
            GetStackChan().rightNeonLight().setColor(r, g, b);

            return true;
        });

    mclog::tagInfo(_tag, "add robot.create_reminder tool");
    mcp_server.AddTool("self.robot.create_reminder",
                       "Create a reminder. Duration is in seconds. Message is what to say when time is up. Set repeat "
                       "to true to repeat the reminder.",
                       PropertyList({Property("duration_seconds", kPropertyTypeInteger, 60, 1, 86400),
                                     Property("message", kPropertyTypeString, std::string("Time's up!")),
                                     Property("repeat", kPropertyTypeBoolean, false)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int duration_seconds = properties["duration_seconds"].value<int>();
                           std::string message  = properties["message"].value<std::string>();
                           bool repeat          = properties["repeat"].value<bool>();

                           // Default message
                           if (message.empty()) {
                               message = "Time's up!";
                           }

                           mclog::tagInfo(_tag, "create_reminder: duration={}s, message={}, repeat={}",
                                          duration_seconds, message, repeat);
                           _show_tool_subtitle("create_reminder", "执行中");

                           int id = tools::create_reminder(duration_seconds * 1000, message, repeat);

                           return id;
                       });

    mclog::tagInfo(_tag, "add robot.get_reminders tool");
    mcp_server.AddTool("self.robot.get_reminders", "Get list of active reminders.", std::vector<Property>{},
                       [this](const PropertyList& properties) -> ReturnValue {
                           mclog::tagInfo(_tag, "get_reminders");
                           _show_tool_subtitle("get_reminders", "查询中");
                           auto reminders          = tools::get_active_reminders();
                           std::string result_json = "[";
                           for (size_t i = 0; i < reminders.size(); ++i) {
                               const auto& r = reminders[i];
                               result_json +=
                                   fmt::format(R"({{"id": {}, "duration_ms": {}, "message": "{}", "repeat": {}}})",
                                               r.id, r.durationMs, r.message, r.repeat ? "true" : "false");
                               if (i < reminders.size() - 1) {
                                   result_json += ", ";
                               }
                           }
                           result_json += "]";
                           mclog::tagInfo(_tag, "get_reminders result: {}", result_json);
                           return result_json;
                       });

    mclog::tagInfo(_tag, "add robot.stop_reminder tool");
    mcp_server.AddTool("self.robot.stop_reminder", "Stop a reminder by ID.",
                       PropertyList({Property("id", kPropertyTypeInteger, -1)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int id = properties["id"].value<int>();
                           mclog::tagInfo(_tag, "stop_reminder: id={}", id);
                           _show_tool_subtitle("stop_reminder", "执行中");
                           tools::stop_reminder(id);
                           return true;
                       });
}

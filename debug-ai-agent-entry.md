# Debug Session: ai-agent-entry

Status: [OPEN]

## Symptoms
- `AI.AGENT -> StackChan` enters but has no bottom Home entry.
- `AI.AGENT -> StackChan` cannot wake with "Hi StackChan".
- `AI.AGENT -> Volcengine` freezes immediately and the device remains frozen.

## Hypotheses
- H1: `StackChan` menu action does not actually trigger `requestXiaozhiStart()`.
- H2: Xiaozhi starts but blocks or starves the Home/status update task.
- H3: Home indicator is created but hidden behind another LVGL layer or not receiving touch.
- H4: Volcengine startup performs blocking network/RTC work on Mooncake UI thread.
- H5: Volcengine startup deadlocks on audio/display resources after previous backend state.

## Evidence Plan
- Capture serial logs during current frozen state and after reset.
- Add temporary instrumentation only if serial logs are insufficient.
- Verify pre-fix and post-fix behavior before cleanup.

## Instrumentation
- Added `[dbg-ai-agent-entry]` serial probes in `AppAiAgent`, `Hal::startXiaozhi()`, `_stackchan_update_task()`, and `volc_agent::start()`.

## Evidence
- Captured `AI.AGENT on open`, then `Volcengine selected`.
- Captured `before startNetwork`, `WiFi scanning...`, `WiFi connecting...`.
- Device then crashed with `Guru Meditation Error: Core 0 panic'ed (LoadProhibited)` before `after startNetwork`.

## Fix Under Test
- Menu callbacks now only set a pending backend.
- Backend action runs after `_menu_page->update()` returns, avoiding self-destruction during UI callback.
- Volcengine startup runs in `volc_start` task, so network/RTC startup does not block the Mooncake UI loop.
- Enabled the `Hi StackChan` wake word model in sdkconfig defaults.
- Volcengine startup failure now requests closing the App instead of staying on the blank page.

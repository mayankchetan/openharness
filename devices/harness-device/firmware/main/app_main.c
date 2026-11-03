// Harness dial — entry point and boot.
//
// Board: the Harness device (ESP32-S3, 466x466 round AMOLED).
//
//   BOOT ─▶ display ─▶ UI ─▶ USB cable link ─▶ the daemon's agent list ─▶ tileview
//
// The way in is the cable. Plugging the dial into a computer running the harness daemon IS the authorization,
// and everything the screen shows arrives over that wire. One more way exists, off by default: Wi-Fi mode (wl_mode in NVS,
// set over the cable), where the same frames travel inside TLS-PSK to the daemon's listener (net_link.c). Neither mode
// holds a backend socket, an account or a pairing step.
//
// What that deleted, and why the boot is a dozen lines instead of a state machine: the SoftAP setup
// portal, the saved-networks walk, the cool-standby retry loop, SNTP, the REST pairing calls, the E2EE
// identity, the OTA download mode and the WS reconnect ladder all existed to get a device onto a network
// and prove who it was. A cable answers both questions by existing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_capture.h"
#include "audio_client.h"
#include "board_pins.h"
#include "cable_client.h"
#include "config_store.h"
#include "fw_update.h"
#include "last_words.h"
#include "ble_link.h"
#include "mode_glue.h"
#include "net_link.h"
#include "board/board.h"
#include "board/board_probe.h"
#include "board/power.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "boot_key.h"
#include "ptt.h"
#include "power_off.h"
#include "power_sleep.h"
#include "ram_telemetry.h"
#ifdef DEVICE_VOX_PROBE
#include "vox_probe.h"
#endif
#include "task_util.h"
#ifdef DEVICE_UI_METRICS
#include "ui/habitat/uim.h"
#endif
#ifdef DEVICE_UI_GALLERY
#include "ui/habitat/ui_gallery.h"
#endif
#ifdef DEVICE_BATTERY_LOG
#include "battery_log.h"
#endif
#ifdef DEVICE_SLEEP_PROFILE
#include "sleep_profile.h"
#endif
#ifdef DEVICE_XIP_PROBE
#include "xip_probe.h"
#endif
#include "ui/display.h"
#include "ui/touch.h"
#include "ui/ui_screens.h"
#include "ui/ui_perf.h"

static const char *TAG = "app";

// Most agents one refresh handles. The link's own ceiling is CABLE_MAX_AGENTS; this is the scratch buffer
// that carries them from its cache into the UI.
#define MAX_PROJECTS CABLE_MAX_AGENTS

// ── factory reset ───────────────────────────────────────────────────────────────────────────────────

// BOOT held through power-on starts a countdown on the glass; the settings (the screen brightness and the voice language — the
// credentials this used to wipe do not exist any more) are cleared only after BK_RESET_HOLD_MS (5 s) of unbroken holding, and letting go
// cancels it (plan 16.1 item 3: with BOOT now the hold-to-talk key, a thumb on it at power-on is not a request to wipe anything).
//
// WHICH button, and whether there is one at all, is the board's to answer (board.h): the dial reads BOOT,
// and the Pro has nothing it could read, because its only switch is the one that just turned it on.
static void reset_countdown(void)
{
    if (!board_factory_reset_requested()) return;
    bkr_t r;
    bkr_begin(&r, (uint32_t)(esp_timer_get_time() / 1000), true);
    ESP_LOGW(TAG, "BOOT held at power-on: settings wipe in %d s unless released", BK_RESET_HOLD_MS / 1000);
    for (;;) {
        const bkr_out_t o = bkr_step(&r, (uint32_t)(esp_timer_get_time() / 1000), board_boot_down());
        if (o.result == BKR_COUNTING) {
            if (o.changed) {
                char text[64];
                snprintf(text, sizeof text, "Keep holding BOOT: %d s. Let go to cancel.", o.seconds_left);
                ui_show_error("Reset settings", text);
            }
        } else if (o.result == BKR_CANCELLED) {
            ESP_LOGI(TAG, "BOOT released: settings kept");
            ui_enter_boot_loading();
            return;
        } else {   // BKR_DONE
            ESP_LOGW(TAG, "BOOT held %d s — factory reset", BK_RESET_HOLD_MS / 1000);
            config_clear_all();
            ui_show_error("Settings reset", "Let go of BOOT.");
            // Restart only when the key is up, however long that takes: a restart under a held BOOT would start the countdown again and wipe again, for ever.
            while (board_boot_down()) vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();   // the UI has already read the old settings; a clean boot reads the cleared ones
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ── the agent list ──────────────────────────────────────────────────────────────────────────────────

// Only ever touched by refresh_task, so one lazily-allocated PSRAM block rather than a stack array — a
// project_t is not small, and sixteen of them have no business on a task stack.
static project_t *proj_scratch(void)
{
    static project_t *s_scratch;
    if (!s_scratch) s_scratch = heap_caps_calloc(MAX_PROJECTS, sizeof(project_t), MALLOC_CAP_SPIRAM);
    return s_scratch;
}

// Pull the agent list and (re)label the tiles.
//
// No RPC and no waiting: the list is whatever the daemon last pushed over the cable. That is what makes
// the old generation/staleness machinery unnecessary — there is no in-flight answer that can arrive after
// the question stopped mattering.
static int refresh_projects(void)
{
    project_t *pr = proj_scratch();
    if (!pr) { ESP_LOGW(TAG, "project scratch alloc failed"); return -1; }

    cable_agent_snapshot_t snapshot;
    const int n = cable_client_list_agents_snapshot(pr, MAX_PROJECTS, &snapshot);
    if (n < 0) return -1; // agents.end will request another refresh after the complete roster arrives
    ESP_LOGI(TAG, "agents: %d", n);

    // Apply the WHOLE reconcile atomically. With the huge-range circular carousel each add/remove
    // re-anchors the scroll to keep the viewed agent centred; locking per ui_* call (as they do
    // internally) lets the LVGL task render between them and the viewed tile visibly wobbles.
    //
    // ...and the bulk bracket is why holding the lock that long is affordable. A cable reconnect empties
    // the list and refills it, so this function runs the add and remove paths once per agent; painting in
    // each of them cost ~130ms, which on a 78-agent dial held the display lock for about ten seconds and
    // tripped the task watchdog into a reboot. Inside the bracket the loops below move the MODEL only and
    // the view catches up once, at bulk_end, on the page the person was already looking at.
    display_lock();
    ui_projects_bulk_begin();

    for (int i = 0; i < n; i++) {
        ui_project_set_name(pr[i].id, pr[i].name);
        ui_project_set_engine(pr[i].id, pr[i].engine);
        ui_project_set_machine(pr[i].id, pr[i].machine_id, pr[i].machine);

        ui_project_reconcile_selected_model(pr[i].id, pr[i].selected_model);
    }

    // Reconcile removals from the end so ui_project_remove() can shift the model without requiring a
    // static snapshot of every current id.
    for (int i = ui_project_count() - 1; i >= 0; i--) {
        char current_id[ID_MAX];
        if (!ui_project_id_at(i, current_id, sizeof(current_id))) continue;
        bool present = false;
        for (int j = 0; j < n; j++) if (strcmp(current_id, pr[j].id) == 0) { present = true; break; }
        if (!present) {
            ESP_LOGI(TAG, "reconcile: removing stale tile %s", current_id);
            ui_project_remove(current_id);
        }
    }

    // Order LAST, inside the same lock-free window as the reconcile above: ui_project_set_name appends,
    // so without this the carousel keeps whatever order it first saw these agents in — which is not the
    // order the daemon sent, and not the order every other surface shows.
    static const char *ids[MAX_PROJECTS];
    for (int i = 0; i < n; i++) ids[i] = pr[i].id;

    ui_projects_bulk_end();   // one re-anchor and one window rebuild for the whole reconcile
    display_unlock();

    if (n > 1) ui_project_apply_order(ids, n);
    // The fleet behind the list: the overview's count, and whether the empty page means "no window" or
    // "empty tab".
    ui_fleet_set(snapshot.total, snapshot.window);
    ui_workspace_applied(snapshot.tab, snapshot.generation);

    // The list is built. If a landing is pending, LAND: drop the loading spinner and focus agent 1 (or the
    // No-agents page). No-op otherwise, so a periodic refresh never yanks the view.
    ui_land_after_reload();
    // ...and if the window asked for a specific agent while that agent was still off the carousel, land
    // THAT — it is a direct instruction and outranks the generic landing above. See ui_focus_project:
    // the daemon pushes the ring then the focus, but this task applies the ring a tick later, so the
    // focus arrives first by construction and has to wait here for the list it named.
    ui_apply_pending_focus();
    ram_telemetry_periodic("refresh");
    return n;
}

// ── the one background task ─────────────────────────────────────────────────────────────────────────

static void refresh_task(void *arg)
{
    (void)arg;
    ui_set_reload_waiter(xTaskGetCurrentTaskHandle());   // …so a pushed list wakes this instead of waiting
    refresh_projects();   // boot-time list: empty until the first push, which is the honest state to show
    while (1) {
        // WOKEN BY THE LIST, not merely polled for it. The second-long sleep is still the floor for the
        // periodic work below, but a list the daemon just pushed applies in milliseconds now. That delay
        // was not neutral: the daemon sends the ring and then the focus that lands on it, so every such
        // focus arrived at a carousel still holding the ring from before — which is the whole of "the
        // window moved to an agent and the dial stayed put". The hold in ui_focus_project makes that
        // correct; this makes it quick.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(power_sleep_wait_ms(PP_POLLER_REFRESH, 1000)));   // 5 s while the chip may sleep

        // Telemetry that must not run in the render task: the flash mmap sample (can wait for a flash write)
        // and the 61 s / 300 s RAM_REGIONS lines. Cheap when nothing is due.
        ram_telemetry_background_tick();
#ifdef DEVICE_UI_METRICS
        uim_tick();   // the per-view UIM line (at most one a second); heap walk and log call stay off the renderer
#endif

        // Chip and settings actions, tapped on the LVGL task and executed here off it.
        ui_service_model_picker();

        // A machine.select the daemon never answered. Drained here rather than on the LVGL task so the
        // bounce (spinner down, wheel back, toast) happens off the render path, like every other action.
        ui_tick_machine_select();

        // The daemon pushed a new list (or the session came up). Peek first so a reload requested mid
        // voice-upload is preserved and retried next tick rather than consumed while the mic is busy.
        if (ui_peek_agent_reload_req() && !audio_client_active() && ui_take_agent_reload_req()) {
            refresh_projects();
        }

        // Reap any "Working…" tile orphaned by a session drop that swallowed its turn-done. Only fires
        // after a re-converge grace, so a live turn is never cut short.
        ui_prune_stale_busy();
    }
}

// ── boot ────────────────────────────────────────────────────────────────────────────────────────────

#ifdef DEVICE_UI_GALLERY
// THE UI GALLERY BOOT (ui/habitat/ui_gallery.inc): the display, the renderer, touch, the UI and the cable link, and
// nothing else. No audio or codecs, no ptt, no beep, no refresh task, no agent logic, no OTA confirmation, no radio, no
// factory-reset check (it writes NVS).
//
// DEVICE_UI_GALLERY_NO_DISPLAY is the same image with the display, renderer, touch, the UI model and the tour not
// STARTED. It is a volatile run-time switch on purpose: a compile-time #if would let the linker drop the whole UI and the
// two images would no longer have the same static memory. RAM tag=gallery_ready is logged at the same point in both, so
// the difference in int_free between the two boots is what starting the UI costs in internal RAM (the static part is
// the same in both and is in `idf.py size`). The tour task starts after that line; its own stack is not counted.
// The same initial value in both images (so the variable sits in the same section and the static memory is identical);
// the NO_DISPLAY image clears it first thing in gallery_main.
static volatile bool s_gallery_ui = true;
void gallery_ram_report(void)
{
    ram_telemetry_checkpoint("gallery_ready");
    ESP_LOGI(TAG, "GALLERY ui=%d tour_stack_b=%d", (int)s_gallery_ui, s_gallery_ui ? UI_GALLERY_TOUR_STACK : 0);
}
static void gallery_main(void)
{
#ifdef DEVICE_UI_GALLERY_NO_DISPLAY
    s_gallery_ui = false;
#endif
    ESP_LOGW(TAG, "UI GALLERY image, ui=%d: not a product build", (int)s_gallery_ui);
    if (s_gallery_ui) {
        display_init();
        ui_init();
        ui_set_brightness(config_load_brightness());
        ram_telemetry_checkpoint("ui_ready");
    }
    if (!cable_client_start()) ESP_LOGE(TAG, "cable link did not start: no daemon can see this dial");
    gallery_ram_report();   // before the daemon session: console only; the cable client logs it again once the session is up
    if (s_gallery_ui) ui_gallery_start();
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
#ifdef DEVICE_XIP_PROBE
    xip_probe_banner();   // diagnostic build only: first thing on the console
#endif
    ram_telemetry_init();   // cJSON nodes and strings go to PSRAM; before any cJSON use (internal fallback is kept)
    ram_telemetry_checkpoint("boot");
    last_words_boot();   // before the first log line, so the previous boot's ring is read, not overwritten
    board_detect();      // which dial this is — the panel's reset pin comes from here, so before display_init
    power_sleep_init();  // DEVICE_BATTERY_SLEEP only (a no-op otherwise): the awake lock, so nothing sleeps until the policy says so
    config_store_init();
    power_off_boot_report();   // DEVICE_BATTERY_POWEROFF only (a no-op otherwise): the AXP2101 source bytes, and why the last boot ended
    power_log_rails();         // the AXP2101 rail registers 0x80..0x9A, read only, one log line (which rails are on is not known for this board)

#ifdef DEVICE_UI_GALLERY
    gallery_main();
    return;
#endif

#ifdef BOARD_PROBE
    board_probe_run();   // bring-up only — see board/board_probe.c
#endif
    display_init();
    ui_init();
    ui_set_brightness(config_load_brightness());
    ram_telemetry_checkpoint("ui_ready");
    reset_countdown();   // BOOT held at power-on: a 5 s countdown on the glass, then the settings wipe; released earlier, nothing happens

    // Reserve the PSRAM voice buffer now, while the heap is still unfragmented — a large contiguous block
    // is hard to get later even with megabytes free.
    audio_client_init();
    ram_telemetry_checkpoint("voice_buffer_ready");

    // Speaker notify: beep on a finished turn. Init before the link so the beep task exists when the first
    // summary arrives.
    audio_notify_init();

    // Enable physical PTT after codec initialization. Otherwise holding the
    // button during boot can race the microphone's first allocation.
    ptt_start();

    // Agents have not arrived yet — don't flash the "No agents" empty tile. Park on the loading screen and
    // arm a landing so refresh_projects slides to agent 0 once the first list lands.
    ui_enter_boot_loading();

#ifdef DEVICE_PERF_BENCH
    ui_perf_run();  // synthetic, local-only, before the cable can affect a live harness
#endif

    // Which way in. mode_glue_boot() decides (task W15; mode_switch.h boot table): NVS says wl_mode = 1 with a whole set of Wi-Fi settings (net.set over
    // the cable put them there), or it does not; stay_wl, boot_usb and whether a host is driving the USB port settle the rest. It runs HERE, after the
    // factory-reset countdown above (which can wipe what it reads) and long after the 20 ms the SOF flag needs to be believed. If the answer is Wi-Fi,
    // net_link_start() brings the radio up and the link runs over Wi-Fi; if the radio does not come up it returns false having started nothing, and the
    // link is the USB cable in this same boot. A change of mode is a reboot, and the mode switch task started below asks for it.
    // The same for Bluetooth (the phone mode, B4b): ble_link_start() runs only when the decision is Bluetooth, brings the BLE host up and starts the link
    // task; on an image without NimBLE (no DEVICE_BLE_LINK) it is an inline stub that returns false and the decision falls back to USB, as for a radio that
    // does not come up.
    const ms_mode_t boot_mode = mode_glue_boot();
    const bool wifi_mode = boot_mode == MS_MODE_WIFI && net_link_start();
    const bool ble_mode = boot_mode == MS_MODE_BLE && ble_link_start();
    if (!cable_client_start_with(wifi_mode ? net_link_xport() : ble_mode ? ble_link_xport() : cable_xport_usb())) {
        // A dial with no link still runs, still lights up, and still says so on screen. A device stuck in a
        // boot loop is not diagnosable from across the room; one showing "Not connected" is.
        ESP_LOGE(TAG, "cable link did not start — the dial will show no machine");
    }
    // After an automatic switch: the agent the dial was showing (held until the roster arrives), and then the task that decides the next switch, told
    // the mode that REALLY started. The UI exists (ui_init above) and the link is up, which is what the busy gate and the daemon-byte clock read.
    mode_glue_restore_focus();
    mode_glue_start(wifi_mode ? MS_MODE_WIFI : ble_mode ? MS_MODE_BLE : MS_MODE_USB);

    // If this boot is a freshly installed image, confirm it HERE and nowhere earlier — see fw_mark_valid.
    // Both things that make the dial fixable have now happened: it can draw, and the port is open so a
    // daemon can reach it to offer another image.
    fw_mark_valid();

    // Create the refresh task BEFORE the first tiles exist: tile creation fragments the remaining internal
    // heap enough that a contiguous stack allocation can fail afterwards, which would leave the carousel on
    // the loading spinner forever because nobody drains ui_request_agent_reload().
#if TASK_STACK_PSRAM_PROJ
    // Stack in PSRAM (task_util.h). No config_/nvs_/esp_ota/esp_partition/esp_flash/spi_flash call is reachable
    // from refresh_task (call-graph grep, 2026-10-28), and it never exits.
    BaseType_t rt_ok = task_create_psram(refresh_task, "proj_refresh", 7168, NULL, 4, NULL, tskNO_AFFINITY);
#else
    BaseType_t rt_ok = xTaskCreate(refresh_task, "proj_refresh", 7168, NULL, 4, NULL);
#endif
    if (rt_ok != pdPASS) {
        ESP_LOGE(TAG, "refresh task create failed (%d), free_int=%u largest_int=%u; rebooting",
                 (int)rt_ok,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
#ifdef DEVICE_BATTERY_LOG
    battery_log_start();   // after the cable link, so a dump reaches the daemon's dial log
#endif
    ram_telemetry_checkpoint("app_ready");
#ifdef DEVICE_VOX_PROBE
    vox_start();   // 100 voice turns over the real path, 30 s after the cable session is up (vox_probe.c); never a daily image
#endif
#ifdef DEVICE_SLEEP_PROFILE
    sleep_profile_app_ready();
    sleep_profile_start();
#endif
#ifdef DEVICE_XIP_PROBE
    xip_probe_start();   // diagnostic build only; see xip_probe.c
#endif
#ifdef DEVICE_SLEEP_PROFILE
    sleep_profile_main_end();   // the main task's stack mark just before it ends
#endif
    vTaskDelete(NULL);
}

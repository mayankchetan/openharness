#include "mode_glue.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cable_client.h"
#include "config_store.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fw_update.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "last_words.h"
#include "mode_glue_core.h"
#include "mode_record.h"
#include "ui/display.h"
#include "ui/ui_screens.h"
#include "usb_probe.h"

static const char *TAG = "mode_sw";

// The record carries an agent id and the module sizes its buffer for one: the same size, checked here and by test_mode_glue_source.py.
_Static_assert(MS_FOCUS_MAX == ID_MAX, "mode_switch.h MS_FOCUS_MAX must equal cable_client.h ID_MAX");
_Static_assert(MS_WL_WIFI == 1 && MS_WL_UNSET == 0, "config_load_wl_effective() returns 1 for Wi-Fi and 0 for none");

#define TASK_STACK 4096          // internal: NVS writes (boot_usb), the driver install, ESP_LOG; high-water mark logged once a minute while the probe runs
#define TASK_PRIO 3
#define IDLE_MS 250              // no probe: one step per MS_STEP_MS
#define PROBE_READ_MS 40         // probe on: read for up to this long between steps, so a burst from the daemon is drained quickly
#define PROBE_RETRY_MS 5000      // the driver would not install: try again this often (a dial that cannot probe cannot come back to USB)
#define RESTART_FLUSH_MS 150     // let the last log lines out before the reset

static mg_boot_out_t s_boot;
static mg_t s_mg;
static ms_mode_t s_running = MS_MODE_USB;
static char s_focus[MS_FOCUS_MAX];     // the agent to restore, copied out of the boot plan
static bool s_focus_pending;
static atomic_bool s_reload;           // mode_glue_settings_changed(): the task re-reads wl_mode and stay_wl at its next step

static uint64_t uptime_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

// ── boot ────────────────────────────────────────────────────────────────────────────────────────────

ms_mode_t mode_glue_boot(void)
{
    mg_boot_in_t in;
    memset(&in, 0, sizeof in);
    // The record first: only a software reset makes it believable, and the reason is read from the chip, not from anything stored.
    in.record_ok = mode_record_boot(esp_reset_reason() == ESP_RST_SW, &in.record);
    in.wl_effective = config_load_wl_effective();
    in.stay_wl = config_load_stay_wl();
    in.boot_usb = config_load_boot_usb();
    // The SOF flag starts TRUE in secondary init and falls only after ticks have run: read it no earlier than MS_SOF_SETTLE_MS after the reset.
    // By the time app_main gets here the display is up and the uptime is in hundreds of milliseconds, so this wait is normally zero.
    uint64_t up = uptime_ms();
    while (up < MS_SOF_SETTLE_MS) {
        vTaskDelay(1);
        up = uptime_ms();
    }
    in.sof = usb_serial_jtag_is_connected();
    in.sof_read_ms = (uint32_t)uptime_ms();
    mg_boot(&in, &s_boot);
    if (s_boot.write_boot_usb && !config_save_boot_usb(s_boot.boot_usb_new))
        ESP_LOGW(TAG, "boot_usb could not be stored; the next boot may pick the other mode (the flap cap bounds it)");
    snprintf(s_focus, sizeof s_focus, "%s", s_boot.focus);
    s_focus_pending = s_focus[0] != 0;
    ESP_LOGI(TAG, "boot: %s (wl_effective=%u stay_wl=%d boot_usb=%d sof=%d at %u ms, record %s flaps=%u)%s", ms_mode_name(s_boot.mode),
             (unsigned)in.wl_effective, (int)in.stay_wl, (int)in.boot_usb, (int)in.sof, (unsigned)in.sof_read_ms,
             in.record_ok ? "restored" : "none", (unsigned)s_boot.flaps, s_boot.unsupported ? " [wireless target unsupported: USB]" : "");
    return s_boot.mode;
}

void mode_glue_restore_focus(void)
{
    if (!s_focus_pending) return;
    s_focus_pending = false;
    ui_focus_project(s_focus);   // an agent not on the carousel yet is held until the roster lands (ui_apply_pending_focus)
    ESP_LOGI(TAG, "focus restored after a mode switch");
}

// ── "never mid-action" ──────────────────────────────────────────────────────────────────────────────

uint32_t mode_switch_busy(void)
{
    // The rows of the user's list and the proposed ones that live in the UI (voice, draft, question, reader, pickers) are ui_mode_switch_busy().
    uint32_t busy = ui_mode_switch_busy(NULL, 0);
    if (cable_client_models_waiting()) busy |= MS_BUSY_MODEL_LIST;
    if (cable_client_pet_upload_active()) busy |= MS_BUSY_PET_UPLOAD;
#ifndef DEVICE_NO_OTA
    if (fw_update_active()) busy |= MS_BUSY_FW_UPDATE;
#endif
    return busy;
}

bool mode_switch_blocked(void) { return ms_blocked(mode_switch_busy(), display_is_asleep()); }

// ── the task ────────────────────────────────────────────────────────────────────────────────────────

static void op_probe(void *ctx, bool on)
{
    (void)ctx;
    if (on) {
        if (!usb_probe_start(uptime_ms())) ESP_LOGW(TAG, "probe did not start; retrying every %d s", PROBE_RETRY_MS / 1000);
    } else {
        usb_probe_stop();
    }
}

static void op_notice(void *ctx, bool on) { (void)ctx; ui_mode_notice(on); }

static void op_clear_flaps(void *ctx)
{
    (void)ctx;
    mode_record_clear_flaps();
    ESP_LOGI(TAG, "10 minutes in one mode: the switch count is cleared");
}

static void op_log_reason(void *ctx, ms_mode_t target)
{
    (void)ctx;
    last_words_note_reason(MS_REASON_SWITCH);
    ESP_LOGW(TAG, "%s: %s -> %s", MS_REASON_SWITCH, ms_mode_name(s_running), ms_mode_name(target));
}

static void op_seal(void *ctx, uint8_t flaps)
{
    (void)ctx;
    char focus[MS_FOCUS_MAX] = "";
    ui_mode_switch_busy(focus, sizeof focus);   // the focused agent, copied under the display lock
    mode_record_save(focus, flaps);
}

static bool op_store_boot_usb(void *ctx, bool boot_usb)
{
    (void)ctx;
    const bool ok = config_save_boot_usb(boot_usb);
    if (!ok) ESP_LOGW(TAG, "boot_usb=%d could not be stored; restarting anyway (the flap cap bounds the detour)", (int)boot_usb);
    return ok;
}

static void op_restart(void *ctx, ms_mode_t target)
{
    (void)ctx;
    (void)target;
    ESP_LOGI(TAG, "stack free_min=%u B (restart)", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    usb_probe_stop();
    vTaskDelay(pdMS_TO_TICKS(RESTART_FLUSH_MS));
    esp_restart();
}

static const mg_ops_t OPS = {
    .ctx = NULL,
    .probe = op_probe,
    .notice = op_notice,
    .clear_flaps = op_clear_flaps,
    .log_reason = op_log_reason,
    .seal = op_seal,
    .store_boot_usb = op_store_boot_usb,
    .restart = op_restart,
};

static void sample(mg_world_t *w, uint64_t now)
{
    memset(w, 0, sizeof *w);
    w->now_ms = now;
    w->sof = usb_serial_jtag_is_connected();
    if (s_running == MS_MODE_USB) {
        w->session_up = cable_client_is_connected();
        uint32_t last32;
        w->have_daemon_byte = cable_client_last_daemon_byte(&last32);
        // The client keeps the low 32 bits of the millisecond clock; the delta is wrap-safe and gives the full 64-bit time.
        if (w->have_daemon_byte) w->last_daemon_byte_ms = now - (uint32_t)((uint32_t)now - last32);
    } else {
        w->have_welcome = usb_probe_last_welcome(&w->last_welcome_ms);
    }
    w->busy = mode_switch_busy();
    w->asleep = display_is_asleep();
}

static void mode_sw_task(void *arg)
{
    (void)arg;
    uint64_t next_step = 0, next_retry = 0, next_mark = uptime_ms() + 10000;
    for (;;) {
        const uint64_t now = uptime_ms();
        if (atomic_exchange(&s_reload, false)) mg_set_settings(&s_mg, config_load_wl_effective(), config_load_stay_wl());

        if (s_mg.probe_on) {
            // The probe is wanted. If the driver would not install, say so and try again: a dial that cannot probe cannot come back to USB.
            if (!usb_probe_running() && now >= next_retry) {
                next_retry = now + PROBE_RETRY_MS;
                (void)usb_probe_start(now);
            }
            usb_probe_poll(now, PROBE_READ_MS);
        } else {
            vTaskDelay(pdMS_TO_TICKS(IDLE_MS));
        }
        // The stack was sized without a measurement (4,096 B, internal): the high-water mark goes in the log 10 s after start, then every 10 minutes,
        // and just before a restart, so the bench can read it and the size can be cut to what is used.
        if (now >= next_mark) {
            next_mark = now + 600000;
            ESP_LOGI(TAG, "stack free_min=%u B", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        const uint64_t t = uptime_ms();
        if (t >= next_step) {
            next_step = t + MS_STEP_MS;
            mg_world_t w;
            sample(&w, t);
            mg_step(&s_mg, &w, &OPS);
        }
    }
}

void mode_glue_start(ms_mode_t running)
{
    s_running = running == MS_MODE_WIFI ? MS_MODE_WIFI : MS_MODE_USB;
    mg_init(&s_mg, s_running, s_boot.flaps, config_load_wl_effective(), config_load_stay_wl());
    if (xTaskCreate(mode_sw_task, "mode_sw", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS)
        ESP_LOGE(TAG, "mode_sw task create failed: this dial will not switch modes by itself");
    else
        ESP_LOGI(TAG, "running in %s mode (wl_effective=%u stay_wl=%d)", ms_mode_name(s_running), (unsigned)s_mg.wl_effective, (int)s_mg.stay_wl);
}

void mode_glue_settings_changed(void) { atomic_store(&s_reload, true); }

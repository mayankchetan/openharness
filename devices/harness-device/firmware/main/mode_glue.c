#include "mode_glue.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "ble_link.h"
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
#include "power_sleep.h"
#include "store_gate.h"
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
#define RESTART_FREEZE_MS 500    // how long an NVS writer in flight is waited for before the reset (power_off.c waits 2,000 ms before cutting the power)

static mg_boot_out_t s_boot;
static mg_t s_mg;
static ms_mode_t s_running = MS_MODE_USB;
static char s_focus[MS_FOCUS_MAX];     // the agent to restore, copied out of the boot plan
static bool s_focus_pending;
static atomic_bool s_keep_wifi;        // stay_wl as of the last read (mode_glue_keep_wifi)
static atomic_bool s_reload;           // mode_glue_settings_changed(): the task re-reads wl_mode and stay_wl at its next step
static bool s_crash_usb;               // this boot took USB because of the panic loop (said again once the daemon is likely to be listening)
static uint8_t s_crashes;              // the crash count as of this boot

static uint64_t uptime_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

// ── boot ────────────────────────────────────────────────────────────────────────────────────────────

// How the panic loop (mode_switch.h) sees a reset. The sets are the user's rule of 2026-10-31 (a crash is any reset that is not a power-on, a software
// restart or a deep-sleep wake) with one deliberate exception: a reset somebody asked for (the reset pin, a flash tool over USB, a debugger) holds the
// count like a software restart does, because three flashes in a row are not three crashes. Every enumerator is named so that a reason a later IDF
// adds is a decision here, not an accident; the default is "crash" (test_mode_glue_source.py checks the sets).
static ms_reset_t reset_class(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return MS_RESET_POWERON;
    case ESP_RST_SW:
    case ESP_RST_DEEPSLEEP:
    case ESP_RST_EXT:
    case ESP_RST_USB:
    case ESP_RST_JTAG:
        return MS_RESET_HOLD;
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_UNKNOWN:
    case ESP_RST_SDIO:
    case ESP_RST_EFUSE:
    case ESP_RST_PWR_GLITCH:
    case ESP_RST_CPU_LOCKUP:
    default:
        return MS_RESET_CRASH;
    }
}

ms_mode_t mode_glue_boot(void)
{
    mg_boot_in_t in;
    memset(&in, 0, sizeof in);
    // The record first: only a software reset makes it believable, and the reason is read from the chip, not from anything stored.
    in.record_ok = mode_record_boot(esp_reset_reason() == ESP_RST_SW, &in.record);
    // The crash record is believed after every reset but a power-on (it is there to count the abnormal ones).
    in.crash_trip = mode_record_crash_boot(reset_class(esp_reset_reason()), &in.crashes);
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
    // At once, with the mode that is about to start: a crash while the radio comes up is counted against Wi-Fi. mode_glue_start() seals again with
    // the mode that really started.
    mode_record_crash_seal(s_boot.crashes, s_boot.mode);
    s_crash_usb = s_boot.crash_usb;
    s_crashes = s_boot.crashes;
    if (s_crash_usb) ESP_LOGW(TAG, "Wireless mode crashed %u times; started in USB", (unsigned)s_crashes);
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
    if (ble_link_pairing_busy()) busy |= MS_BUSY_PAIRING;   // B5: the pairing window is open or its number is on the screen (stub: false)
#ifndef DEVICE_NO_OTA
    if (fw_update_active()) busy |= MS_BUSY_FW_UPDATE;
#endif
    return busy;
}

bool mode_switch_blocked(void) { return ms_gate_blocked(s_running, mode_switch_busy(), display_is_asleep(), display_idle_ms()); }

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

static void op_held(void *ctx, ms_mode_t target, uint32_t busy, bool asleep)
{
    (void)ctx;
    ESP_LOGI(TAG, "switch to %s is due and held: busy=%#x (hard %#x) %s", ms_mode_name(target), (unsigned)busy, (unsigned)(busy & MS_BUSY_HARD),
             asleep ? "screen off: only an update or an upload holds a dark dial" : "screen on: waits for the screen to go off");
}

static void op_clear_crashes(void *ctx)
{
    (void)ctx;
    mode_record_crash_clear();
    s_crashes = 0;
    ESP_LOGI(TAG, "10 minutes up in a wireless mode: the crash count is cleared");
}

// A refused restart: the sequence had written the target's boot_usb and a raised flap count. Put both back, so a reset that comes for another reason
// finds the dial as it was; the reason noted for last_words is blanked too (it said "mode switch"). `boot_usb_before` is what NVS held before the
// sequence wrote it (the core read it through op_load_boot_usb): after a radio fallback that is the CHOICE (Wi-Fi), not the mode that runs. The
// core starts the sequence again after a delay (mg_retry_delay_ms).
static void op_undo(void *ctx, bool boot_usb_before, uint8_t flaps)
{
    (void)ctx;
    last_words_note_reason("");
    if (!config_save_boot_usb(boot_usb_before)) ESP_LOGW(TAG, "boot_usb could not be put back after the refused restart");
    mode_record_save(NULL, flaps);
}

static bool op_load_boot_usb(void *ctx)
{
    (void)ctx;
    return config_load_boot_usb();
}

// The store gate and the busy gate behind the restart, as mg_restart_try wants them (the order and the refusals are mode_glue_core.c's and are run
// on the host in test_mode_glue.c: already frozen by power_off.c is skipped and never thawed here; a writer that will not finish; busy at the last look).
static bool gate_frozen(void *c) { (void)c; return store_gate_frozen(); }
static bool gate_freeze(void *c) { (void)c; return store_gate_freeze(RESTART_FREEZE_MS); }
static void gate_thaw(void *c) { (void)c; store_gate_thaw(); }
static bool gate_blocked(void *c) { (void)c; return mode_switch_blocked(); }
static void gate_restart(void *c) { (void)c; esp_restart(); }
static const mg_gate_ops_t GATE = {NULL, gate_frozen, gate_freeze, gate_thaw, gate_blocked, gate_restart};

static bool op_restart(void *ctx, ms_mode_t target)
{
    (void)ctx;
    (void)target;
    ESP_LOGI(TAG, "stack free_min=%u B (restart)", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    usb_probe_stop();
    vTaskDelay(pdMS_TO_TICKS(RESTART_FLUSH_MS));
    // The sequence before this took a display lock, an NVS commit and 150 ms, and the user may have started a voice turn, a draft or a pet upload
    // meanwhile. Stop the NVS writers first (as power_off.c does before it cuts the power: no write is half done when the reset comes), then look at
    // the gate one last time, hard rows and soft rows by the same rule as the step (ms_blocked). Nothing but esp_restart() follows the look.
    switch (mg_restart_try(&GATE)) {
    case MG_RESTART_GONE:
        break;   // not reached: esp_restart() does not return
    case MG_RESTART_FROZEN:
        ESP_LOGW(TAG, "restart skipped: the NVS writers are frozen for a power-off; the gate is left as found");
        break;
    case MG_RESTART_WRITER:
        ESP_LOGW(TAG, "restart postponed: an NVS writer did not finish in %d ms", RESTART_FREEZE_MS);
        break;
    case MG_RESTART_BUSY:
        ESP_LOGW(TAG, "restart called off: the dial turned busy at the last moment (busy=%#x); it will try again", (unsigned)mode_switch_busy());
        break;
    }
    return false;
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
    .clear_crashes = op_clear_crashes,
    .undo = op_undo,
    .load_boot_usb = op_load_boot_usb,
    .held = op_held,
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
    w->idle_ms = display_idle_ms();
}

static void mode_sw_task(void *arg)
{
    (void)arg;
    uint64_t next_step = 0, next_retry = 0, next_mark = uptime_ms() + 10000;
    for (;;) {
        const uint64_t now = uptime_ms();
        // A reload (the Link screen wrote wl_mode or stay_wl), and again at every loop while a drop to "no target" waits for its second read: the
        // value of a read taken inside a config_net_set_mode write is not believed alone (mg_apply_settings, MG_CONFIRM_MS).
        if (atomic_exchange(&s_reload, false) || s_mg.wl_pending) {
            mg_apply_settings(&s_mg, config_load_wl_effective(), config_load_stay_wl(), now);
            atomic_store(&s_keep_wifi, s_mg.stay_wl);
        }

        if (s_mg.probe_on) {
            // The probe is wanted. If the driver would not install, say so and try again: a dial that cannot probe cannot come back to USB.
            if (!usb_probe_running() && now >= next_retry) {
                next_retry = now + PROBE_RETRY_MS;
                (void)usb_probe_start(now);
            }
            usb_probe_poll(now, PROBE_READ_MS);
        }
        // Every pass waits. A running probe waits in usb_probe_poll (its read). Without the probe, or with the probe wanted but not installed (it reads
        // nothing and usb_probe_poll returns at once: without this wait the task would spin, starve the idle task and trip its watchdog), the task
        // sleeps here: IDLE_MS, or PROBE_READ_MS when the probe is wanted (the step cadence a running probe gives), and the relaxed poll (2 s) while the
        // chip may sleep, since there is nothing to read.
        if (!s_mg.probe_on || !usb_probe_running())
            vTaskDelay(pdMS_TO_TICKS(power_sleep_wait_ms(PP_POLLER_MODE_SW, s_mg.probe_on ? PROBE_READ_MS : IDLE_MS)));
        // The stack was sized without a measurement (4,096 B, internal): the high-water mark goes in the log 10 s after start, then every 10 minutes,
        // and just before a restart, so the bench can read it and the size can be cut to what is used.
        if (now >= next_mark) {
            next_mark = now + 600000;
            ESP_LOGI(TAG, "stack free_min=%u B", (unsigned)uxTaskGetStackHighWaterMark(NULL));
            // The panic loop's reason, again: the boot line may have gone out before a daemon was listening.
            if (s_crash_usb) {
                s_crash_usb = false;
                ESP_LOGW(TAG, "Wireless mode crashed %u times; started in USB", (unsigned)s_crashes);
            }
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
    s_running = ms_mode_is_wireless(running) ? running : MS_MODE_USB;
    // A dial that RUNS a wireless mode was told by the boot decision that this mode is a target, so it starts believing that (the stored byte is the
    // mode number); the first loop re-reads (and a read that says "no target" has to be confirmed like any other), so one failed read of NVS here
    // cannot send it to USB (review finding 2).
    mg_init(&s_mg, s_running, s_boot.flaps, ms_mode_is_wireless(s_running) ? (uint8_t)s_running : config_load_wl_effective(), config_load_stay_wl());
    if (ms_mode_is_wireless(s_running)) atomic_store(&s_reload, true);
    atomic_store(&s_keep_wifi, s_mg.stay_wl);
    mg_note_crashes(&s_mg, s_crashes);
    mode_record_crash_seal(s_crashes, s_running);   // the mode that really started (the radio may not have come up: then USB, and a crash is not Wi-Fi's)
    if (xTaskCreate(mode_sw_task, "mode_sw", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS)
        ESP_LOGE(TAG, "mode_sw task create failed: this dial will not switch modes by itself");
    else
        ESP_LOGI(TAG, "running in %s mode (wl_effective=%u stay_wl=%d)", ms_mode_name(s_running), (unsigned)s_mg.wl_effective, (int)s_mg.stay_wl);
}

bool mode_glue_keep_wifi(void) { return atomic_load(&s_keep_wifi); }

void mode_glue_settings_changed(void) { atomic_store(&s_reload, true); }

"""The PMIC power-off sequence (main/power_off.c + main/store_gate.c, DEVICE_BATTERY_POWEROFF), run on the host against fake IDF headers.

The production power_off.c and store_gate.c are compiled unchanged, with fakes for FreeRTOS, NVS, the PMIC, the screen, the USB host and the
firmware update. Every fake appends a token to one event log, so the ORDER of the sequence is checked (plan 16.1 item 2):

    one log line -> writers stopped (store gate frozen) -> NVS record committed -> flash settle -> "Off - hold PWR" on the glass for 1 s
    -> REG 0x10 bit 0 (the PMIC write)

Also checked:
  * NEVER on external power: VBUS good, an unreadable REG 0x00, or a USB host refuses the request before anything is touched (no freeze, no
    write, no PMIC write); the debug trigger (force) skips those two checks only, and still refuses during a firmware update;
  * a writer in flight is waited for; one that does not finish in time stops the power-off and opens the gate again;
  * a PMIC that does not take the write (the dial is still running 3 s later): the gate opens, the screen goes back, a retry waits;
  * one request at a time, a retry throttle after a refusal;
  * the record: reason, percent, mV, uptime; the next boot logs it once and erases it; the boot log carries REG 0x20 and 0x21;
  * the store gate: writers are counted, a frozen gate refuses, a freeze waits for the writers and times out when one stays.
"""
from pathlib import Path
import os
import subprocess
import tempfile

fw = Path(__file__).resolve().parent / '..'
main = fw / 'main'

FAKES = {
'fake_idf.h': '''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NVS_NOT_FOUND 0x1102
extern char g_events[8192];
extern char g_logs[16384];
static inline void ev(const char *s) { size_t n = strlen(g_events); if (n + strlen(s) + 2 < sizeof g_events) { strcat(g_events, s); strcat(g_events, " "); } }
#include <stdarg.h>
__attribute__((format(printf, 1, 2))) static inline int logf_(const char *fmt, ...)
{
    char line[512];
    va_list ap; va_start(ap, fmt); (void)vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    size_t n = strlen(g_logs);
    if (n + strlen(line) + 2 < sizeof g_logs) { strcat(g_logs, line); strcat(g_logs, "\\n"); }
    return 0;
}
#define ESP_LOGI(tag, ...) ((void)(tag), logf_(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), logf_(__VA_ARGS__))
#define ESP_LOGE(tag, ...) ((void)(tag), logf_(__VA_ARGS__))
''',
'esp_log.h': '#pragma once\n#include "fake_idf.h"\n',
'esp_timer.h': '#pragma once\n#include "fake_idf.h"\nextern int64_t g_now_us;\nstatic inline int64_t esp_timer_get_time(void) { return g_now_us; }\n',
'freertos/FreeRTOS.h': '''
#pragma once
#include "../fake_idf.h"
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
typedef int BaseType_t;
typedef void *TaskHandle_t;
''',
'freertos/task.h': '''
#pragma once
#include "FreeRTOS.h"
void vTaskDelay(uint32_t ticks);
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio, TaskHandle_t *out);
void vTaskDelete(TaskHandle_t t);
''',
'nvs.h': '''
#pragma once
#include "fake_idf.h"
typedef int nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t n);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *n);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_commit(nvs_handle_t h);
void nvs_close(nvs_handle_t h);
''',
'board/power.h': '''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define POWER_STATUS0_VBUS_GOOD     0x20
#define POWER_STATUS0_BATT_PRESENT  0x08
int power_pmu_read(int *status0, int *status1);
bool power_pmic_soft_off(void);
int power_read_reg(uint8_t reg);
''',
'battery.h': '#pragma once\nint battery_pct_raw(void);\nint battery_mv(void);\n',
'driver/usb_serial_jtag.h': '#pragma once\n#include <stdbool.h>\nbool usb_serial_jtag_is_connected(void);\n',
'fw_update.h': '#pragma once\n#include <stdbool.h>\nbool fw_update_active(void);\n',
'ui/display.h': '#pragma once\n#include <stdbool.h>\nvoid display_lock_at(const char *who);\n#define display_lock() display_lock_at(__func__)\nvoid display_unlock(void);\nbool display_is_asleep(void);\nvoid display_wake(void);\n',
'ui/ui_screens.h': '#pragma once\nvoid ui_show_error(const char *title, const char *detail);\nvoid ui_leave_error_screen(void);\n',
}

HARNESS = r'''
#include "fake_idf.h"
#include "power_off.h"
#include "power_policy.h"
#include "store_gate.h"
#include "freertos/task.h"
#include "nvs.h"
#include "board/power.h"
#include <assert.h>
#include <setjmp.h>

char g_events[8192];
char g_logs[16384];
int64_t g_now_us = 1000000;

// ---- fake world ----
static int status0 = 0x08, status1 = 0x40;       // on the battery: VBUS not good, cell present, discharging
static bool usb_host, ota, asleep = true, soft_off_ok = true, dies_on_soft_off = true, nvs_fail;
static int pct = 4, mv = 3300, reg20 = 0x04, reg21 = 0x02, soft_off_calls, delays;
static jmp_buf death;                            // the PMIC cuts the power: the task never runs another line
static bool armed_death;
static uint32_t delay_total_ms;
static int plug_during_show;                    // 1: VBUS appears while the screen is shown; 2: a USB host does
static int writer_in_flight_ms;                  // a writer holds the gate for this long (virtual time), then leaves; <0: for ever

int power_pmu_read(int *s0, int *s1) { ev("PMUREAD"); *s0 = status0; *s1 = status1; return status1 < 0 ? -1 : (status1 >> 5) & 3; }
bool power_pmic_soft_off(void) { ev("REG10"); soft_off_calls++; if (dies_on_soft_off && soft_off_ok && armed_death) longjmp(death, 1); return soft_off_ok; }
int power_read_reg(uint8_t reg) { return reg == 0x20 ? reg20 : reg == 0x21 ? reg21 : -1; }
int battery_pct_raw(void) { return pct; }
int battery_mv(void) { return mv; }
bool usb_serial_jtag_is_connected(void) { return usb_host; }
bool fw_update_active(void) { return ota; }
void display_lock_at(const char *w) { (void)w; ev("LOCK"); }
void display_unlock(void) { ev("UNLOCK"); }
bool display_is_asleep(void) { return asleep; }
void display_wake(void) { asleep = false; ev("WAKE"); }
void ui_show_error(const char *t, const char *d) { char b[96]; snprintf(b, sizeof b, "SHOW[%s|%s]", t, d); ev(b); }
void ui_leave_error_screen(void) { ev("HOME"); }

// NVS: one namespace, one record, in memory; "commit" is the moment it becomes durable
static struct { bool present; uint8_t data[32]; size_t len; } store, pending;
static int open_mode;
static int64_t t_set;   // virtual time of the record write
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; if (nvs_fail) return ESP_FAIL; open_mode = m; pending = store; *h = 1; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n) { (void)h; (void)k; assert(open_mode == NVS_READWRITE && n <= sizeof pending.data); memcpy(pending.data, v, n); pending.len = n; pending.present = true; t_set = g_now_us; ev("NVSSET"); return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *n) { (void)h; (void)k; if (!pending.present) return ESP_ERR_NVS_NOT_FOUND; if (*n < pending.len) return ESP_FAIL; memcpy(v, pending.data, pending.len); *n = pending.len; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { (void)h; (void)k; if (!pending.present) return ESP_ERR_NVS_NOT_FOUND; pending.present = false; ev("NVSERASE"); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; store = pending; ev("NVSCOMMIT"); return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

// FreeRTOS: a created task runs to its end at once (it deletes itself); the delays add up virtual time
static int tasks_created;
static bool defer;                               // a created task waits until run_deferred() (to look at the busy flag)
static void (*deferred_fn)(void *);
static void *deferred_arg;
static void run_task(void (*fn)(void *), void *arg) {
    if (armed_death && setjmp(death)) { ev("DEAD"); return; }
    fn(arg);
}
void vTaskDelay(uint32_t ms) { if (ms == 1000 && plug_during_show) { if (plug_during_show == 1) status0 = 0x28; else usb_host = true; } delays++; delay_total_ms += ms; g_now_us += (int64_t)ms * 1000; if (writer_in_flight_ms >= 0 && writer_in_flight_ms > 0) { writer_in_flight_ms -= (int)ms; if (writer_in_flight_ms <= 0) { writer_in_flight_ms = 0; store_gate_leave(); ev("WRITER-DONE"); } } }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio, TaskHandle_t *out) {
    (void)name; (void)stack; (void)prio; (void)out; tasks_created++; ev("TASK");
    if (defer) { deferred_fn = fn; deferred_arg = arg; return pdPASS; }
    run_task(fn, arg);
    return pdPASS;
}
static void run_deferred(void) { void (*f)(void *) = deferred_fn; deferred_fn = NULL; run_task(f, deferred_arg); }
void vTaskDelete(TaskHandle_t t) { (void)t; }

static void clear_events(void) { g_events[0] = 0; g_logs[0] = 0; }
static bool in_order(const char *a, const char *b) { const char *pa = strstr(g_events, a); return pa && strstr(pa + strlen(a), b); }
static bool has(const char *s) { return strstr(g_events, s) != NULL; }
static int count_of(const char *needle) { int n = 0; for (const char *p = g_events; (p = strstr(p, needle)); p += strlen(needle)) n++; return n; }
static void reset_world(void) {
    status0 = 0x08; status1 = 0x40; usb_host = ota = false; asleep = true; soft_off_ok = true; dies_on_soft_off = true; nvs_fail = false;
    pct = 4; mv = 3300; reg20 = 0x04; reg21 = 0x02; soft_off_calls = delays = 0; delay_total_ms = 0; writer_in_flight_ms = 0; plug_during_show = 0; armed_death = true;
    defer = false; store.present = pending.present = false; tasks_created = 0; g_now_us += 10LL * 1000000 * 100;   // far past any retry throttle
    store_gate_thaw();
    clear_events();
}

typedef struct { uint32_t magic; uint8_t reason; int8_t pct; uint16_t mv; uint32_t up_s; } rec_t;   // what power_off.c writes (12 bytes)

static void scenario_sequence(void) {
    reset_world();
    assert(power_off_request(PP_OFF_LOW_BATTERY, false));
    // the whole order of 16.1 item 2, and the PMIC write is the last thing that happens
    assert(in_order("TASK", "PMUREAD") && in_order("PMUREAD", "NVSSET") && in_order("NVSSET", "NVSCOMMIT") && in_order("NVSCOMMIT", "WAKE")
           && in_order("WAKE", "SHOW[Power|Off - hold PWR]") && in_order("SHOW[Power|Off - hold PWR]", "REG10"));
    assert(has("DEAD") && !strstr(strstr(g_events, "REG10"), "NVSSET"));      // nothing after the PMIC write
    // exactly one log line before the power-off, with the reason and the numbers
    const char *line = strstr(g_logs, "POWEROFF reason=low_battery pct=4 mv=3300 force=0");
    assert(line && strstr(line + 1, "POWEROFF reason=") == NULL);
    assert(line < strstr(g_logs, "PMIC soft power-off written") || !strstr(g_logs, "PMIC soft power-off written"));
    // the writers were stopped before the record: a config write after the freeze is refused
    assert(store_gate_frozen() && !store_gate_enter());
    // the screen is shown for 1 s before the PMIC write
    assert(delay_total_ms >= 1000);
    // the record: reason, percent, mV, uptime, durable (committed) and with the magic
    assert(store.present && store.len == sizeof(rec_t));
    rec_t rec; memcpy(&rec, store.data, sizeof rec);
    assert(rec.magic == 0x504F4646u && rec.reason == PP_OFF_LOW_BATTERY && rec.pct == 4 && rec.mv == 3300 && rec.up_s == (uint32_t)(t_set / 1000000));
}

static void scenario_never_on_external_power(void) {
    const struct { int s0; int s1; bool usb; const char *what; } cases[] = {
        {0x28, 0x40, false, "VBUS good (REG 0x00 bit 5)"},
        {0x2C, 0x40, false, "VBUS good with a cell and a rising state"},
        {-1, 0x40, false, "REG 0x00 unreadable"},
        {0x08, 0x40, true, "a USB host"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_world();
        status0 = cases[i].s0; status1 = cases[i].s1; usb_host = cases[i].usb;
        assert(power_off_request(PP_OFF_S2_TIMEOUT, false));              // the request starts the task ...
        assert(has("PMUREAD"));                                           // ... which looks once, fresh, before anything else
        assert(!has("NVSSET") && !has("WAKE") && !has("REG10") && !has("SHOW") && soft_off_calls == 0);   // ... and stops there
        assert(!store.present && !store_gate_frozen() && store_gate_enter());
        store_gate_leave();
        assert(strstr(g_logs, "refused"));
    }
    // a firmware update refuses the policy's request, and that refusal arms the same one-minute throttle as the others (no task a poll)
    reset_world();
    ota = true;
    assert(power_off_request(PP_OFF_S2_TIMEOUT, false) && tasks_created == 1);
    assert(!has("NVSSET") && !has("REG10") && strstr(g_logs, "firmware update"));
    assert(!power_off_request(PP_OFF_S2_TIMEOUT, false) && tasks_created == 1);
    g_now_us += 61LL * 1000000;
    assert(power_off_request(PP_OFF_S2_TIMEOUT, false) && tasks_created == 2);
    // a firmware update refuses even the forced one
    reset_world();
    ota = true;
    assert(power_off_request(POWER_OFF_REASON_DEBUG, true));
    assert(!has("NVSSET") && !has("REG10") && !store_gate_frozen());
    // the forced one (the bench trigger) skips the VBUS and USB checks and nothing else
    reset_world();
    dies_on_soft_off = false;   // (a death would leave the busy flag set: this process goes on to more scenarios)
    status0 = 0x28; usb_host = true;
    assert(power_off_request(POWER_OFF_REASON_DEBUG, true));
    assert(!has("PMUREAD"));
    assert(in_order("NVSCOMMIT", "SHOW[Power|Off - hold PWR]") && in_order("SHOW[Power|Off - hold PWR]", "REG10"));
    assert(strstr(g_logs, "POWEROFF reason=debug pct=4 mv=3300 force=1"));
    // a refusal arms the throttle: the policy asks again at once, the answer is no until a minute has passed
    reset_world();
    status0 = 0x28;
    assert(power_off_request(PP_OFF_S2_TIMEOUT, false));
    assert(!power_off_request(PP_OFF_S2_TIMEOUT, false) && tasks_created == 1);
    g_now_us += 61LL * 1000000;
    assert(power_off_request(PP_OFF_S2_TIMEOUT, false) && tasks_created == 2);
}

static void scenario_writer_in_flight(void) {
    // a writer that finishes within the wait: the sequence waits for it, then goes on
    reset_world();
    dies_on_soft_off = false;
    assert(store_gate_enter());
    writer_in_flight_ms = 300;
    assert(power_off_request(PP_OFF_LOW_BATTERY, false));
    assert(in_order("WRITER-DONE", "NVSSET") && in_order("NVSSET", "REG10"));
    // a writer that never finishes: no record, no PMIC write, the gate opens again and the writer can leave
    reset_world();
    store_gate_thaw();
    assert(store_gate_enter());
    writer_in_flight_ms = -1;
    assert(power_off_request(PP_OFF_LOW_BATTERY, false));
    assert(!has("NVSSET") && !has("REG10") && !store_gate_frozen());
    assert(strstr(g_logs, "writer did not finish"));
    store_gate_leave();
    assert(store_gate_enter()); store_gate_leave();
}

static void scenario_pmic_does_not_take_it(void) {
    // the write is accepted but the dial keeps running (VBUS keeps it up on a forced run): the gate opens, the screen goes back
    reset_world();
    dies_on_soft_off = false;
    status0 = 0x28;
    assert(power_off_request(POWER_OFF_REASON_DEBUG, true));
    assert(in_order("REG10", "HOME") && !store_gate_frozen() && store_gate_enter());
    store_gate_leave();
    assert(strstr(g_logs, "still running"));
    assert(delay_total_ms >= 1000 + 3000);
    // and a second request is possible afterwards
    assert(power_off_request(POWER_OFF_REASON_DEBUG, true) && tasks_created == 2);
    // a failed PMIC write is logged and treated the same
    reset_world();
    soft_off_ok = false;
    assert(power_off_request(PP_OFF_DRAIN, false));
    assert(strstr(g_logs, "FAILED") && strstr(g_logs, "still running") && !store_gate_frozen());
}

static void scenario_plugged_during_screen(void) {
    // VBUS (or a USB host) appears during the 1 s of "Off - hold PWR": the PMIC write is NOT made, the gate opens again, the screen goes back
    for (int what = 1; what <= 2; what++) {
        reset_world();
        dies_on_soft_off = true;
        plug_during_show = what;
        assert(power_off_request(PP_OFF_S2_TIMEOUT, false));
        assert(in_order("SHOW[Power|Off - hold PWR]", "HOME") && !has("REG10") && soft_off_calls == 0);
        assert(count_of("PMUREAD") == 2 && in_order("SHOW[Power|Off - hold PWR]", "PMUREAD"));   // looked once before, once after the screen
        assert(!store_gate_frozen() && store_gate_enter());
        store_gate_leave();
        assert(strstr(g_logs, "refused") && strstr(g_logs, "still running") == NULL);
        // and an unforced retry waits a minute
        assert(!power_off_request(PP_OFF_S2_TIMEOUT, false));
        // reset_world() of the next round moves the clock past the throttle; the busy flag is clear (the task ended)
        assert(!power_off_pending());
    }
    // the bench trigger (force) does not look again: it is meant to run on a cable
    reset_world();
    dies_on_soft_off = false;
    plug_during_show = 1;
    assert(power_off_request(POWER_OFF_REASON_DEBUG, true));
    assert(has("REG10") && !has("PMUREAD"));
}

static void scenario_one_at_a_time(void) {
    reset_world();
    defer = true;
    status0 = 0x28;                                                        // (the task will refuse when it runs: no PMIC write)
    assert(power_off_request(PP_OFF_DRAIN, false) && power_off_pending() && tasks_created == 1);
    assert(!power_off_request(PP_OFF_DRAIN, false) && !power_off_request(POWER_OFF_REASON_DEBUG, true));   // busy: even the forced one waits
    assert(tasks_created == 1);
    run_deferred();
    assert(!power_off_pending());
    defer = false;
    assert(power_off_request(POWER_OFF_REASON_DEBUG, true) && tasks_created == 2);
}

static void scenario_nvs_failure(void) {
    // the record is a convenience: if NVS refuses it the power-off still goes ahead
    reset_world();
    nvs_fail = true;
    assert(power_off_request(PP_OFF_S2_TIMEOUT, false));
    assert(strstr(g_logs, "record") && has("REG10"));
}

static void scenario_boot_report(void) {
    reset_world();
    assert(power_off_request(PP_OFF_DRAIN, false));                        // writes the record
    clear_events();
    power_off_boot_report();
    assert(strstr(g_logs, "reason=drain pct=4 mv=3300"));                  // logged once ...
    assert(strstr(g_logs, "REG 0x20=0x4 REG 0x21=0x2"));                   // ... with the two source bytes
    assert(in_order("NVSERASE", "NVSCOMMIT") && !store.present);           // ... and erased
    clear_events();
    power_off_boot_report();
    assert(!strstr(g_logs, "reason=drain") && strstr(g_logs, "REG 0x20"));  // the second boot says nothing about it
    // a record with a wrong magic is ignored and erased
    reset_world();
    store.present = true; store.len = sizeof(rec_t); memset(store.data, 0xA5, store.len);
    power_off_boot_report();
    assert(!strstr(g_logs, "previous boot"));
}

static void scenario_gate(void) {
    reset_world();
    store_gate_thaw();
    assert(!store_gate_frozen() && store_gate_enter() && store_gate_enter());
    assert(!store_gate_freeze(50));                                        // two writers, neither leaves: times out ...
    assert(!store_gate_frozen());                                          // ... and the gate is open again
    store_gate_leave(); store_gate_leave();
    assert(store_gate_freeze(50) && store_gate_frozen());                  // no writer: frozen at once
    assert(!store_gate_enter());
    store_gate_thaw();
    assert(store_gate_enter()); store_gate_leave();
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "sequence")) scenario_sequence();
    else if (!strcmp(argv[1], "external")) scenario_never_on_external_power();
    else if (!strcmp(argv[1], "writer")) scenario_writer_in_flight();
    else if (!strcmp(argv[1], "pmic")) scenario_pmic_does_not_take_it();
    else if (!strcmp(argv[1], "plug")) scenario_plugged_during_screen();
    else if (!strcmp(argv[1], "one")) scenario_one_at_a_time();
    else if (!strcmp(argv[1], "nvs")) scenario_nvs_failure();
    else if (!strcmp(argv[1], "boot")) scenario_boot_report();
    else if (!strcmp(argv[1], "gate")) scenario_gate();
    else assert(!"unknown scenario");
    (void)count_of;
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='harness-power-off-') as folder:
    folder = Path(folder)
    for name, text in FAKES.items():
        path = folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    # production sources are compiled from COPIES beside the fake headers (a quoted include looks in the including file's directory first)
    for name in ('power_off.c', 'store_gate.c', 'power_policy.c'):
        (folder / name).write_text((main / name).read_text())
    (folder / 'harness.c').write_text(HARNESS)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-O1', '-g', '-DDEVICE_BATTERY_SLEEP=1',
                    '-DDEVICE_BATTERY_POWEROFF=1',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    '-I', str(main), str(folder / 'harness.c'), str(folder / 'power_off.c'), str(folder / 'store_gate.c'),
                    str(folder / 'power_policy.c'), '-o', str(folder / 'poff')], check=True)
    scenarios = ('sequence', 'external', 'writer', 'pmic', 'plug', 'one', 'nvs', 'boot', 'gate')
    for name in scenarios:
        subprocess.run([str(folder / 'poff'), name], check=True)
    print('power off: ' + ', '.join(scenarios) + ' PASS (log line, writers stopped, NVS record committed, "Off - hold PWR", PMIC write last; '
          'never on VBUS or a USB host; writer in flight; PMIC that stays up; one request at a time; boot report)')

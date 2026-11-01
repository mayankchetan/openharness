"""The sleep breadcrumb and the watchdog's starved list (last_words.c, DEVICE_BATTERY_SLEEP), run on the host.

The production last_words.c is compiled in (included by the harness, so the test can reach its RTC variables) against fakes of the IDF headers.
"Reboots" are calls to last_words_boot() with a different reset reason: the statics stand in for the RTC memory that survives a reset.

Checked:
  * a breadcrumb written by the ptt task every wake (tick, policy state wanted and applied, S2 applied, light sleeps, holds, render stage) is kept
    across an abnormal reset and reported once, as one log line, by last_words_report();
  * after a normal reset (power-on, software restart) it is dropped and not reported;
  * a torn or foreign record (bad checksum, bad magic) is not believed;
  * esp_task_wdt_isr_user_handler() (called from the task watchdog's interrupt just before the panic) records which entries starved, as text, into
    the RTC record, and the next boot reports it; the report says "no entry" when the handler found none;
  * with the flag off (the default image) none of this is compiled: no breadcrumb function, no handler, no extra log lines.
"""
from pathlib import Path
import os
import subprocess
import tempfile

main = Path(__file__).resolve().parent / '../main'

FAKES = {
    'esp_attr.h': '#pragma once\n#define RTC_NOINIT_ATTR\n#define EXT_RAM_BSS_ATTR\n',
    'esp_system.h': r'''
#pragma once
typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT,
               ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB, ESP_RST_JTAG } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
''',
    'freertos/FreeRTOS.h': r'''
#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
''',
    'esp_log.h': r'''
#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
extern char g_logs[8192];
static inline void log_add(const char *fmt, ...)
{
    char line[400];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    strncat(g_logs, line, sizeof g_logs - strlen(g_logs) - 2); strcat(g_logs, "\n");
}
#define ESP_LOGI(tag, ...) ((void)(tag), log_add(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), log_add(__VA_ARGS__))
''',
    'esp_task_wdt.h': r'''
#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
typedef void (*task_wdt_msg_handler)(void *opaque, const char *msg);
esp_err_t esp_task_wdt_print_triggered_tasks(task_wdt_msg_handler msg_handler, void *opaque, int *cpus_fail);
void esp_task_wdt_isr_user_handler(void);
''',
}

HARNESS = r'''
#include "last_words.c"
#include <assert.h>

char g_logs[8192];
static esp_reset_reason_t reason = ESP_RST_POWERON;
esp_reset_reason_t esp_reset_reason(void) { return reason; }
// the watchdog: which entries did not reset it (what the IDF function would print); empty list = ESP_FAIL
static const char *starved_names[4];
static int starved_count;
esp_err_t esp_task_wdt_print_triggered_tasks(task_wdt_msg_handler h, void *opaque, int *cpus_fail)
{
    (void)cpus_fail;
    if (!starved_count) return ESP_FAIL;
    h(opaque, "Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:");
    for (int i = 0; i < starved_count; i++) { h(opaque, "\n - "); h(opaque, starved_names[i]); h(opaque, " (CPU 0)"); }
    return ESP_OK;
}
static void reboot(esp_reset_reason_t r) { reason = r; s_reported = false; s_previous_len = 0; g_logs[0] = 0; last_words_boot(); }
static last_words_crumb_t crumb(uint32_t tick, uint8_t want, uint8_t applied, uint8_t in_s2, uint32_t sleeps, uint32_t holds, uint8_t stage)
{
    last_words_crumb_t c = {.tick_ms = tick, .want = want, .applied = applied, .in_s2 = in_s2, .sleeps = sleeps, .holds = holds, .stage = stage};
    return c;
}

int main(void)
{
    // ---- a crash after wakes: the last crumb is reported, once ----
    reboot(ESP_RST_POWERON);
    last_words_report();
    assert(!strstr(g_logs, "breadcrumb"));
    last_words_crumb_t c1 = crumb(1000, 1, 1, 0, 0, 0x0, 3), c2 = crumb(1250, 2, 2, 1, 7, 0x0, 4), c3 = crumb(1500, 2, 2, 1, 7, 0x8, 5);
    last_words_crumb(&c1); last_words_crumb(&c2); last_words_crumb(&c3);
    starved_names[0] = "IDLE0"; starved_names[1] = "render"; starved_count = 2;
    esp_task_wdt_isr_user_handler();
    reboot(ESP_RST_TASK_WDT);
    last_words_report();
    assert(strstr(g_logs, "reset reason: TASK WATCHDOG"));
    assert(strstr(g_logs, "last sleep breadcrumb: wake #3 at 1500 ms: want S2 applied S2 in_s2=1 sleeps=7 holds=0x8 render_stage=5"));
    assert(strstr(g_logs, "starved at the watchdog:") && strstr(g_logs, "IDLE0 (CPU 0)") && strstr(g_logs, "render (CPU 0)"));
    assert(!strchr(strstr(g_logs, "starved at the watchdog:"), '\r'));
    // reported once
    char before[8192]; strcpy(before, g_logs); last_words_report(); assert(!strcmp(before, g_logs));

    // ---- the record is used once: the next abnormal reset with no new crumb reports nothing about it ----
    reboot(ESP_RST_PANIC);
    last_words_report();
    assert(!strstr(g_logs, "breadcrumb") && !strstr(g_logs, "starved at the watchdog"));

    // ---- a normal reset drops the record ----
    last_words_crumb(&c2);
    reboot(ESP_RST_SW);
    last_words_report();
    assert(!strstr(g_logs, "breadcrumb"));
    last_words_crumb(&c2);
    reboot(ESP_RST_POWERON);
    last_words_report();
    assert(!strstr(g_logs, "breadcrumb"));

    // ---- a torn write (the checksum does not match) is not believed ----
    last_words_crumb(&c2);
    s_crumb.at.tick_ms ^= 0x10;
    reboot(ESP_RST_INT_WDT);
    last_words_report();
    assert(strstr(g_logs, "breadcrumb unreadable") && !strstr(g_logs, "wake #"));
    // foreign memory (no magic): silence, as on a cold boot
    last_words_crumb(&c2);
    s_crumb.magic = 0x12345678u;
    reboot(ESP_RST_WDT);
    last_words_report();
    assert(!strstr(g_logs, "breadcrumb"));

    // ---- the watchdog handler with no starved entry records nothing and does not damage the crumb ----
    last_words_crumb(&c3);
    starved_count = 0;
    esp_task_wdt_isr_user_handler();
    reboot(ESP_RST_TASK_WDT);
    last_words_report();
    assert(strstr(g_logs, "wake #") && strstr(g_logs, "starved at the watchdog: no entry"));

    // ---- a very long list is cut, never overrun ----
    last_words_crumb(&c1);
    static char longname[200]; memset(longname, 'n', sizeof longname - 1);
    starved_names[0] = longname; starved_names[1] = longname; starved_names[2] = longname; starved_count = 3;
    esp_task_wdt_isr_user_handler();
    assert(strlen(s_crumb.starved) < sizeof s_crumb.starved);
    reboot(ESP_RST_TASK_WDT);
    last_words_report();
    assert(strstr(g_logs, "wake #") && strstr(g_logs, "starved at the watchdog: "));
    puts("last words crumb: report once, dropped after a normal reset, torn record refused, starved list recorded and cut PASS");
    return 0;
}
'''

OFF = r'''
#include "last_words.c"
#include <assert.h>
char g_logs[8192];
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_TASK_WDT; }
int main(void)
{
    last_words_boot();
    last_words_report();
    assert(!strstr(g_logs, "breadcrumb") && !strstr(g_logs, "starved"));
    puts("flag off: no breadcrumb code, no watchdog handler PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='harness-last-words-') as folder:
    folder = Path(folder)
    for name, text in FAKES.items():
        path = folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    # the production source is compiled from a COPY beside the fakes (a quoted include looks in the including file's directory first)
    (folder / 'last_words.c').write_text((main / 'last_words.c').read_text())
    (folder / 'last_words.h').write_text((main / 'last_words.h').read_text())
    (folder / 'on.c').write_text(HARNESS)
    (folder / 'off.c').write_text(OFF)
    flags = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'), '-I', str(folder)]
    subprocess.run(flags + ['-DDEVICE_BATTERY_SLEEP=1', str(folder / 'on.c'), '-o', str(folder / 'on')], check=True)
    subprocess.run([str(folder / 'on')], check=True)
    subprocess.run(flags + [str(folder / 'off.c'), '-o', str(folder / 'off')], check=True)
    subprocess.run([str(folder / 'off')], check=True)

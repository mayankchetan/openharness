"""audio_capture_idle(): stop the I2S channels while the chip may sleep; the codec library starts them again when it opens.

The production functions are cut out of audio_capture.c and run against fake I2S channels (with the driver's READY / RUNNING
states and its error log), a fake log-level API and a fake codec lock:
  * idle stops RX, then TX, and leaves them READY (disabled): the state esp_codec_dev_close leaves after every beep and capture, so the
    next esp_codec_dev_open (set_fmt disables, reconfigures the clock and slot from READY, then enables) works exactly as it does
    between two beeps today. Nothing here enables a channel again (finding 4: a RUNNING channel would make that reconfigure fail);
  * stopping a channel that is already stopped (the codec library closed it) does not log an error from the driver ("i2s_common"
    is silenced for the call and restored after);
  * idle(false) only says "audio may start again": it enables nothing;
  * idle is refused (and touches nothing) while a capture is open or requested, and while the codec lock is taken (a beep);
  * neither audio_capture_start nor the beep path touches the channels for the sleep (source-structure check);
  * with the flag off none of it is compiled.
Facts the design stands on, from the sources: i2s_std.c i2s_channel_reconfig_std_clock/slot return ESP_ERR_INVALID_STATE unless the
channel is READY; i2s_common.c i2s_channel_enable needs READY and i2s_channel_disable needs RUNNING, both log with ESP_LOGE otherwise;
esp_codec_dev audio_codec_data_i2s.c _i2s_data_set_fmt disables the channels itself before it reconfigures them.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

main = Path(__file__).resolve().parent / '../main'
source = (main / 'audio_capture.c').read_text()
header = (main / 'audio_capture.h').read_text()


def function(name):
    match = re.search(r'^[^\n]*\b' + name + r'\([^;]*?\)\n\{.*?^\}', source, re.M | re.S)
    assert match, name
    return match.group(0) + '\n'


# --- everything is behind the flag ---
block = re.search(r'#ifdef DEVICE_BATTERY_SLEEP\nstatic bool s_idle;.*?\n#endif\n\nstatic void play_beep', source, re.S)
assert block, 'idle state and functions sit inside #ifdef DEVICE_BATTERY_SLEEP'
assert 'audio_capture_idle(bool idle)' in block[0] and 'i2s_stop_quiet' in block[0]
assert 'i2s_channel_enable' not in block[0].replace('// ', ''), 'the idle code never enables a channel (a RUNNING channel cannot be reconfigured by the codec open)'
assert re.search(r'#ifdef DEVICE_BATTERY_SLEEP\n(?:[^\n]*\n)*?bool audio_capture_idle\(bool idle\);\n#endif', header)

# --- the capture start and the beep path are as they were: the codec open enables what it needs ---
assert 'idle_resume_locked' not in source
beep = function('play_beep')
assert 'DEVICE_BATTERY_SLEEP' not in beep and 'i2s_channel' not in beep
start = function('audio_capture_start')
assert 'DEVICE_BATTERY_SLEEP' not in start
assert start.count('i2s_channel_enable(s_rx)') == 1   # the one that was there: "make sure the RX clock is running"

code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define DEVICE_BATTERY_SLEEP 1
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR } esp_log_level_t;
typedef struct { bool running; } chan_t;
typedef chan_t *i2s_chan_handle_t;
static chan_t rx_chan, tx_chan;
static i2s_chan_handle_t s_rx = &rx_chan, s_tx = &tx_chan;
static int lock_token, *s_codec_lock = &lock_token;
static bool s_open;
static atomic_bool s_capture_requested;
static char calls[64];
static bool lock_busy, lock_held;
static unsigned last_take_timeout, driver_errors;
static esp_log_level_t i2s_level = ESP_LOG_ERROR;
static esp_log_level_t esp_log_level_get(const char *tag) { assert(!strcmp(tag, "i2s_common")); return i2s_level; }
static void esp_log_level_set(const char *tag, esp_log_level_t l) { assert(!strcmp(tag, "i2s_common")); i2s_level = l; }
/* the driver: disable needs RUNNING and logs an error otherwise; enable needs READY (nothing in the code under test calls it) */
static int i2s_channel_disable(i2s_chan_handle_t c) {
    strcat(calls, c == s_rx ? "R-" : "T-");
    if (!c->running) { if (i2s_level != ESP_LOG_NONE) driver_errors++; return 0x103; }
    c->running = false; return 0;
}
static int xSemaphoreTake(int *lock, unsigned timeout) {
    assert(lock == &lock_token); last_take_timeout = timeout;
    if (lock_busy || lock_held) return 0;
    lock_held = true; return pdTRUE;
}
static void xSemaphoreGive(int *lock) { assert(lock == &lock_token && lock_held); lock_held = false; }
static bool s_idle;
''' + function('i2s_stop_quiet') + function('audio_capture_idle') + r'''
static void reset(void) {
    calls[0] = 0; lock_busy = lock_held = false; s_open = false; atomic_store(&s_capture_requested, false);
    rx_chan.running = tx_chan.running = true; driver_errors = 0; i2s_level = ESP_LOG_ERROR; s_idle = false;
}
int main(void) {
    // stop: RX then TX, both end READY; twice is one stop
    reset();
    assert(audio_capture_idle(true) && !strcmp(calls, "R-T-") && s_idle && !lock_held);
    assert(!rx_chan.running && !tx_chan.running && driver_errors == 0 && i2s_level == ESP_LOG_ERROR);
    assert(audio_capture_idle(true) && !strcmp(calls, "R-T-"));
    // "audio may start again" clears the flag and enables nothing: the next codec open does that from READY
    assert(audio_capture_idle(false) && !strcmp(calls, "R-T-") && !s_idle && !lock_held && last_take_timeout == 200);
    assert(!rx_chan.running && !tx_chan.running);
    assert(audio_capture_idle(false) && !strcmp(calls, "R-T-"));
    // channels the codec library already closed (after a beep or a capture): stopping them again logs nothing from the driver
    reset(); rx_chan.running = tx_chan.running = false;
    assert(audio_capture_idle(true) && s_idle && driver_errors == 0 && i2s_level == ESP_LOG_ERROR);
    reset(); tx_chan.running = false;                       // one of two
    assert(audio_capture_idle(true) && driver_errors == 0 && !rx_chan.running);
    // the driver's tag is restored even though it was silenced
    assert(i2s_level == ESP_LOG_ERROR);
    // a capture open or requested: refused, nothing touched
    reset(); s_open = true;
    assert(!audio_capture_idle(true) && !calls[0] && !s_idle && rx_chan.running);
    reset(); atomic_store(&s_capture_requested, true);
    assert(!audio_capture_idle(true) && !calls[0] && !s_idle);
    // the codec lock is taken (a beep is playing, or a capture is opening): refused, and the lock is not left taken
    reset(); lock_busy = true;
    assert(!audio_capture_idle(true) && !calls[0] && !s_idle && last_take_timeout == 0 && rx_chan.running);
    // a refused "start again" says so and keeps the flag
    reset(); s_idle = true; lock_busy = true;
    assert(!audio_capture_idle(false) && s_idle);
    lock_busy = false;
    assert(audio_capture_idle(false) && !s_idle && !calls[0]);
    // audio that never came up: nothing to do
    reset(); s_codec_lock = NULL;
    assert(audio_capture_idle(true) && audio_capture_idle(false) && !calls[0]);
    puts("audio idle: RX then TX stop and stay READY, no driver error for a stopped channel, nothing is enabled again, a capture or a beep refuses the stop PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-audio-idle-') as folder:
    folder = Path(folder)
    (folder / 'idle.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'), str(folder / 'idle.c'),
                    '-o', str(folder / 'idle')], check=True)
    subprocess.run([str(folder / 'idle')], check=True)

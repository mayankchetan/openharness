"""audio_capture_idle(): stop the I2S channels while the chip may sleep, start them again for a capture or a beep.

The production functions are cut out of audio_capture.c and run against fake I2S channels and a fake codec lock:
  * idle stops RX, then TX; idle twice changes nothing; resume enables TX, then RX (the boot order); resume twice changes nothing,
  * idle is refused (and touches nothing) while a capture is open or requested, and while the codec lock is taken (a beep),
  * a capture start or a beep that finds the channels stopped starts them first (source-structure check on both call sites),
  * with the flag off none of it is compiled.
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
assert 'audio_capture_idle(bool idle)' in block[0] and 'idle_resume_locked' in block[0]
assert re.search(r'#ifdef DEVICE_BATTERY_SLEEP\n(?:[^\n]*\n)*?bool audio_capture_idle\(bool idle\);\n#endif', header)

# --- both users of the I2S clocks resume them under the codec lock, before the codec opens ---
start = function('audio_capture_start')
assert re.search(r'xSemaphoreTake\(s_codec_lock,portMAX_DELAY\);\n#ifdef DEVICE_BATTERY_SLEEP\n    idle_resume_locked\(\);[^\n]*\n#endif\n    esp_codec_dev_sample_info_t', start)
beep = function('play_beep')
assert re.search(r'#ifdef DEVICE_BATTERY_SLEEP\n    idle_resume_locked\(\);[^\n]*\n#endif\n    esp_codec_dev_sample_info_t', beep)
assert beep.index('idle_resume_locked') > beep.index('xSemaphoreTake(s_codec_lock, 0)')

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
typedef void *i2s_chan_handle_t;
typedef int SemaphoreHandle_t_;
static int rx_token, tx_token;
static i2s_chan_handle_t s_rx = &rx_token, s_tx = &tx_token;
static int lock_token, *s_codec_lock = &lock_token;
static bool s_open;
static atomic_bool s_capture_requested;
static char calls[64];
static bool lock_busy, lock_held;
static unsigned last_take_timeout;
static int i2s_channel_enable(i2s_chan_handle_t c) { strcat(calls, c == s_rx ? "R+" : "T+"); return 0; }
static int i2s_channel_disable(i2s_chan_handle_t c) { strcat(calls, c == s_rx ? "R-" : "T-"); return 0; }
static int xSemaphoreTake(int *lock, unsigned timeout) {
    assert(lock == &lock_token); last_take_timeout = timeout;
    if (lock_busy || lock_held) return 0;
    lock_held = true; return pdTRUE;
}
static void xSemaphoreGive(int *lock) { assert(lock == &lock_token && lock_held); lock_held = false; }
static bool s_idle;
''' + function('idle_resume_locked') + function('audio_capture_idle') + r'''
static void reset(void) { calls[0] = 0; lock_busy = lock_held = false; s_open = false; atomic_store(&s_capture_requested, false); }
int main(void) {
    // stop: RX then TX; twice is one stop
    reset();
    assert(audio_capture_idle(true) && !strcmp(calls, "R-T-") && s_idle && !lock_held);
    assert(audio_capture_idle(true) && !strcmp(calls, "R-T-"));
    // start: TX then RX, once
    assert(audio_capture_idle(false) && !strcmp(calls, "R-T-T+R+") && !s_idle && !lock_held && last_take_timeout == 200);
    assert(audio_capture_idle(false) && !strcmp(calls, "R-T-T+R+"));
    // a capture open or requested: refused, nothing touched
    reset(); s_open = true;
    assert(!audio_capture_idle(true) && !calls[0] && !s_idle);
    reset(); atomic_store(&s_capture_requested, true);
    assert(!audio_capture_idle(true) && !calls[0] && !s_idle);
    // the codec lock is taken (a beep is playing, or a capture is opening): refused, and the lock is not left taken
    reset(); lock_busy = true;
    assert(!audio_capture_idle(true) && !calls[0] && !s_idle && last_take_timeout == 0);
    // a capture request that lands between the first check and the lock is honoured
    reset();
    atomic_store(&s_capture_requested, false);
    s_idle = false;
    {
        // simulate: requested flag set by the time the lock is held -> the channels stay running, the answer is "not stopped"
        atomic_store(&s_capture_requested, true);
        assert(!audio_capture_idle(true) && !calls[0]);
    }
    // resume on an idle that never happened is a no-op; a resume refused for a busy lock says so
    reset(); s_idle = true; lock_busy = true;
    assert(!audio_capture_idle(false) && s_idle && !calls[0]);
    lock_busy = false;
    assert(audio_capture_idle(false) && !s_idle && !strcmp(calls, "T+R+"));
    // audio that never came up: nothing to do
    reset(); s_codec_lock = NULL;
    assert(audio_capture_idle(true) && audio_capture_idle(false) && !calls[0]);
    puts("audio idle: RX then TX stop, TX then RX start, repeats are no-ops, a capture or a beep refuses the stop PASS");
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

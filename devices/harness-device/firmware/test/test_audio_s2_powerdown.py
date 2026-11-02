"""S2 power-down of the audio parts (2026-11-02): the speaker amplifier enable, and later the two codecs.

The production functions are cut out of audio_capture.c and run against fakes, as test_audio_idle.py does:
  * S2_AMP_OFF: when the I2S clocks are stopped for S2, GPIO 46 (BSP_PA_IO, the NS4150B enable) is driven LOW and kept an output.
    The output latch is written low BEFORE the pad becomes an output, so it cannot pulse high. It is done once per stop, only after
    the stop was accepted, and never while a capture is open or requested or a beep plays (the codec library owns the pin then:
    es8311.c es8311_pa_power raises it in es8311_enable(true) and drops it in es8311_enable(false));
  * with the switch off (default in a plain build) none of it is compiled;
  * nothing else drives the pin high: the beep path is as it was.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent / '..'
main = root / 'main'
source = (main / 'audio_capture.c').read_text()
header = (main / 'audio_capture.h').read_text()


def function(name, text=source):
    match = re.search(r'^[^\n]*\b' + name + r'\([^;]*?\)\n\{.*?^\}', text, re.M | re.S)
    assert match, name
    return match.group(0) + '\n'


# --- the switches header: three names, all default 0, and forced to 0 without the battery sleep ---
switches = (main / 's2_switches.h').read_text()
assert 'S2_AMP_OFF' in switches
assert re.search(r'#ifndef S2_AMP_OFF\n#define S2_AMP_OFF 0\n#endif', switches)
assert 'DEVICE_BATTERY_SLEEP' in switches, 'without the battery sleep the switches are 0'
assert '#include "s2_switches.h"' in source

# --- the amp helper sits behind its switch, inside the battery sleep block ---
assert re.search(r'#if S2_AMP_OFF\n(?://[^\n]*\n)+static void s2_amp_off\(void\)', source)
idle = function('audio_capture_idle')
assert idle.count('s2_amp_off()') == 2, 'once in the stop branch, once when audio never came up'

# --- the beep path and the capture path never mention the switches: the codec library drives the pin there ---
for name in ('play_beep', 'audio_capture_start', 'audio_capture_stop'):
    assert 'S2_' not in function(name), name
assert 'BSP_PA_IO' not in function('play_beep'), 'the beep path leaves the pin to the codec library'
assert not re.search(r'gpio_set_level\(BSP_PA_IO,\s*1\)', source), 'nothing here ever raises the pin'

code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define DEVICE_BATTERY_SLEEP 1
#define S2_AMP_OFF 1
#define BSP_PA_IO 46
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define GPIO_MODE_OUTPUT 2
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR } esp_log_level_t;
typedef struct { bool running; } chan_t;
typedef chan_t *i2s_chan_handle_t;
static chan_t rx_chan, tx_chan;
static i2s_chan_handle_t s_rx = &rx_chan, s_tx = &tx_chan;
static int lock_token, *s_codec_lock = &lock_token;
static bool s_open;
static atomic_bool s_capture_requested;
static char calls[128];
static bool lock_busy, lock_held;
static int pa_level = -1, pa_mode = -1;        // what the pin ended as
static esp_log_level_t i2s_level = ESP_LOG_ERROR;
static esp_log_level_t esp_log_level_get(const char *tag) { (void)tag; return i2s_level; }
static void esp_log_level_set(const char *tag, esp_log_level_t l) { (void)tag; i2s_level = l; }
static int i2s_channel_disable(i2s_chan_handle_t c) {
    strcat(calls, c == s_rx ? "R-" : "T-");
    if (!c->running) return 0x103;
    c->running = false; return 0;
}
static int xSemaphoreTake(int *lock, unsigned timeout) { (void)timeout; assert(lock == &lock_token); if (lock_busy || lock_held) return 0; lock_held = true; return pdTRUE; }
static void xSemaphoreGive(int *lock) { assert(lock == &lock_token && lock_held); lock_held = false; }
static int gpio_set_level(int pin, int level) { assert(pin == BSP_PA_IO); pa_level = level; strcat(calls, level ? "a1" : "a0"); return 0; }
static int gpio_config(const gpio_config_t *c) {
    assert(c->pin_bit_mask == (1ULL << BSP_PA_IO));
    assert(c->pull_up_en == GPIO_PULLUP_DISABLE && c->pull_down_en == GPIO_PULLDOWN_DISABLE && c->intr_type == GPIO_INTR_DISABLE);
    pa_mode = c->mode; strcat(calls, "aC"); return 0;
}
static bool s_idle;
''' + function('i2s_stop_quiet') + function('s2_amp_off') + function('audio_capture_idle') + r'''
static void reset(void) {
    calls[0] = 0; lock_busy = lock_held = false; s_open = false; atomic_store(&s_capture_requested, false);
    rx_chan.running = tx_chan.running = true; i2s_level = ESP_LOG_ERROR; s_idle = false; pa_level = pa_mode = -1;
}
int main(void) {
    // the stop: the latch low, the pad an output, the latch low again, THEN the clocks stop (RX, TX)
    reset();
    assert(audio_capture_idle(true) && !strcmp(calls, "a0aCa0R-T-"));
    assert(pa_level == 0 && pa_mode == GPIO_MODE_OUTPUT && !lock_held);
    // a repeat is one stop: the pin is not touched again
    assert(audio_capture_idle(true) && !strcmp(calls, "a0aCa0R-T-"));
    // "audio may start again" leaves the pin alone: only the codec library raises it, when a beep opens the speaker
    assert(audio_capture_idle(false) && !strcmp(calls, "a0aCa0R-T-") && pa_level == 0);
    // the next S2 stops and drives it low again
    calls[0] = 0; rx_chan.running = tx_chan.running = false;
    assert(audio_capture_idle(true) && !strcmp(calls, "a0aCa0R-T-"));
    // refused (capture open or requested, a beep holding the lock): the pin is not touched
    reset(); s_open = true;
    assert(!audio_capture_idle(true) && !calls[0] && pa_level == -1);
    reset(); atomic_store(&s_capture_requested, true);
    assert(!audio_capture_idle(true) && !calls[0] && pa_level == -1);
    reset(); lock_busy = true;
    assert(!audio_capture_idle(true) && !calls[0] && pa_level == -1 && !s_idle);
    // audio that never came up (no codec lock): the pin is still driven low, once, and the call says "nothing to stop"
    reset(); s_codec_lock = NULL;
    assert(audio_capture_idle(true) && !strcmp(calls, "a0aCa0") && pa_level == 0);
    assert(audio_capture_idle(false) && !strcmp(calls, "a0aCa0"));
    puts("audio S2 power-down (amp): GPIO46 low before the clocks stop, once per stop, never while a capture or a beep owns the pin PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-audio-s2-') as folder:
    folder = Path(folder)
    (folder / 's2.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'), str(folder / 's2.c'),
                    '-o', str(folder / 's2')], check=True)
    subprocess.run([str(folder / 's2')], check=True)

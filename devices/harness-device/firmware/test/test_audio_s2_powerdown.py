"""S2 power-down of the audio parts (2026-11-02): the speaker amplifier enable and the two codecs.

The production functions are cut out of audio_capture.c and run against fakes, as test_audio_idle.py does.

S2_AMP_OFF
  * when the I2S clocks are stopped for S2, GPIO 46 (BSP_PA_IO, the NS4150B enable) is driven LOW and kept an output. The output
    latch is written low BEFORE the pad becomes an output, so it cannot pulse high. It is done once per stop, only after the stop was
    accepted, and never while a capture is open or requested or a beep plays (the codec library owns the pin then: es8311.c
    es8311_pa_power raises it in es8311_enable(true) and drops it in es8311_enable(false));
  * with the switch off (default in a plain build) none of it is compiled; nothing else drives the pin high: the beep path is as it was.

S2_CODEC_OFF
  * the ES7210 (microphone ADC) and ES8311 (speaker codec) are put in the lowest-power state the esp_codec_dev driver itself uses, by
    replaying that driver's own stop sequence (es7210_stop, es8311_suspend) over the register interface;
  * only when a register probe says the codec is still in its powered "opened" state (it is until the first capture or beep after boot;
    esp_codec_dev_close puts it in the stopped state every time after), and BEFORE the I2S clocks stop, because the driver always writes
    those registers while MCLK runs (esp_codec_dev_close disables the codec first and the I2S channel second);
  * never while a capture is open or requested or a beep plays (the stop is refused before anything is touched);
  * the tables equal the driver's own sequences, write for write (checked against the driver source when it is on disk), so the state S2
    leaves is exactly the state every capture and beep close leaves. The next esp_codec_dev_open therefore starts from a state the driver
    already starts from after every previous capture or beep: this is the argument that voice and beep work after an S2 exit;
  * a failed probe or write does not stop the S2 entry.
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


# --- the switches header: every name defaults to 0, and the battery sleep image is what turns them on ---
switches = (main / 's2_switches.h').read_text()
for name in ('S2_AMP_OFF', 'S2_CODEC_OFF'):
    assert re.search(r'#ifndef ' + name + r'\n#define ' + name + r' 0\n#endif', switches), name
    assert '#undef ' + name in switches, name + ' is forced to 0 without DEVICE_BATTERY_SLEEP'
assert 'DEVICE_BATTERY_SLEEP' in switches
assert '#include "s2_switches.h"' in source
cmake = (main / 'CMakeLists.txt').read_text()
loop = re.search(r'foreach\(s2_switch ([A-Z0-9_ ]+)\)', cmake)
assert loop and {'S2_AMP_OFF', 'S2_CODEC_OFF'} <= set(loop.group(1).split()), 'each switch is ON in a battery sleep image unless -D<name>=0'

# --- the helpers sit behind their switches, inside the battery sleep block, before audio_capture_idle ---
assert re.search(r'#if S2_AMP_OFF\n(?://[^\n]*\n)+static void s2_amp_off\(void\)', source)
codec_block = re.search(r'#if S2_CODEC_OFF\n.*?\n#endif\n', source, re.S)
assert codec_block, 'S2_CODEC_OFF block'
idle_at = source.index('bool audio_capture_idle(bool idle)\n{')
assert codec_block.start() < idle_at
idle = function('audio_capture_idle')
assert idle.count('s2_amp_off()') == 2, 'once in the stop branch, once when audio never came up'
assert idle.count('s2_codec_off()') == 1, 'one call, in the stop branch only (the codec handles are not used without the lock)'
assert idle.index('s2_amp_off();') < idle.index('s2_codec_off();') < idle.index('i2s_stop_quiet(s_rx)'), 'amp low, codecs down, THEN the clocks stop'

# --- the beep path and the capture path never mention the switches: the codec library drives the pin and the codecs there ---
for name in ('play_beep', 'audio_capture_start', 'audio_capture_stop', 'audio_notify_init'):
    assert 'S2_' not in function(name), name
assert 'BSP_PA_IO' not in function('play_beep'), 'the beep path leaves the pin to the codec library'
assert not re.search(r'gpio_set_level\(BSP_PA_IO,\s*1\)', source), 'nothing here ever raises the pin'


# --- the tables are the driver's own stop sequences, write for write (when the driver source is on disk) ---
def parse_table(text):
    return [(int(reg, 16), int(val, 16)) for reg, val in re.findall(r'\{0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+)\}', text)]


def table(name):
    match = re.search(r'static const s2_reg_t ' + name + r'\[\] = \{(.*?)\n\};', source, re.S)
    assert match, name
    return parse_table(match.group(1))


es7210_table, es8311_table = table('ES7210_STOP'), table('ES8311_SUSPEND')
assert len(es7210_table) == 9 and len(es8311_table) == 15
driver = root / 'managed_components/espressif__esp_codec_dev/device'
if driver.is_dir():
    def driver_sequence(c_file, h_file, fn, writer):
        defs = dict((n, int(v, 16)) for n, v in re.findall(r'#define\s+(\w+)\s+0x([0-9a-fA-F]+)', (driver / h_file).read_text()))
        body = re.search(r'static int ' + fn + r'\(audio_codec_\w+_t \*codec\)\n\{(.*?)\n\}', (driver / c_file).read_text(), re.S)
        assert body, fn
        return [(defs[n], int(v, 16)) for n, v in re.findall(writer + r'\(codec,\s*(\w+),\s*(0x[0-9a-fA-F]+)\)', body.group(1))]
    assert driver_sequence('es7210/es7210.c', 'es7210/es7210_reg.h', 'es7210_stop', 'es7210_write_reg') == es7210_table
    assert driver_sequence('es8311/es8311.c', 'es8311/es8311_reg.h', 'es8311_suspend', 'es8311_write_reg') == es8311_table
    # the two probe values: the last write of the stop sequence to the probed register
    assert [v for r, v in es7210_table if r == 0x4B][-1] == 0xFF
    assert [v for r, v in es8311_table if r == 0x0D][-1] == 0xFC
    driver_note = 'tables equal es7210_stop and es8311_suspend of the driver on disk'
else:
    driver_note = 'driver source not on disk: table parity skipped'

code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define DEVICE_BATTERY_SLEEP 1
#define BSP_PA_IO 46
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define GPIO_MODE_OUTPUT 2
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define ESP_CODEC_DEV_OK 0
#define ESP_LOGI(tag, ...) do { (void)tag; logged++; } while (0)
#define ESP_LOGW(tag, ...) do { (void)tag; logged++; } while (0)
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR } esp_log_level_t;
typedef struct { bool running; } chan_t;
typedef chan_t *i2s_chan_handle_t;
static chan_t rx_chan, tx_chan;
static i2s_chan_handle_t s_rx = &rx_chan, s_tx = &tx_chan;
static int lock_token, *s_codec_lock = &lock_token;
static bool s_open;
static atomic_bool s_capture_requested;
static char calls[1024];
static unsigned logged;
static bool lock_busy, lock_held;
static int pa_level = -1;
static const char *TAG = "audio_cap";
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
    assert(c->pin_bit_mask == (1ULL << BSP_PA_IO) && c->mode == GPIO_MODE_OUTPUT);
    assert(c->pull_up_en == GPIO_PULLUP_DISABLE && c->pull_down_en == GPIO_PULLDOWN_DISABLE && c->intr_type == GPIO_INTR_DISABLE);
    strcat(calls, "aC"); return 0;
}
/* the two codecs: a register file each, the driver's API (esp_codec_dev_read_reg / _write_reg), and fault injection */
typedef struct { int regs[256]; char tag; int fail_read, fail_write_at, writes; } bank_t;
typedef bank_t *esp_codec_dev_handle_t;
static bank_t mic_bank = { .tag = 'M' }, spk_bank = { .tag = 'S' };
static esp_codec_dev_handle_t s_mic = &mic_bank, s_spk = &spk_bank;
static struct { char dev; int reg, val; } wr[64];
static int nwr;
static int esp_codec_dev_read_reg(esp_codec_dev_handle_t h, int reg, int *v) {
    char t[4] = { 'r', h->tag, 0, 0 }; strcat(calls, t);
    if (h->fail_read) return -1;
    *v = h->regs[reg]; return ESP_CODEC_DEV_OK;
}
static int esp_codec_dev_write_reg(esp_codec_dev_handle_t h, int reg, int val) {
    char t[4] = { 'w', h->tag, 0, 0 }; strcat(calls, t);
    h->writes++;
    if (h->fail_write_at && h->writes == h->fail_write_at) return -1;
    h->regs[reg] = val;
    assert(nwr < 64); wr[nwr].dev = h->tag; wr[nwr].reg = reg; wr[nwr].val = val; nwr++;
    return ESP_CODEC_DEV_OK;
}
static bool s_idle;
''' + function('i2s_stop_quiet') + function('s2_amp_off') + codec_block[0] + function('audio_capture_idle') + r'''
static void reset(void) {
    calls[0] = 0; lock_busy = lock_held = false; s_open = false; atomic_store(&s_capture_requested, false); s_codec_lock = &lock_token;
    rx_chan.running = tx_chan.running = true; i2s_level = ESP_LOG_ERROR; s_idle = false; pa_level = -1; nwr = 0; logged = 0;
    s_mic = &mic_bank; s_spk = &spk_bank;
    memset(mic_bank.regs, 0, sizeof mic_bank.regs); memset(spk_bank.regs, 0, sizeof spk_bank.regs);
    mic_bank.fail_read = spk_bank.fail_read = mic_bank.fail_write_at = spk_bank.fail_write_at = mic_bank.writes = spk_bank.writes = 0;
}
#if S2_CODEC_OFF
static void hot(void)  { mic_bank.regs[0x4B] = 0x00; spk_bank.regs[0x0D] = 0xFA; }   /* after es7210_open / es8311_open: nothing has closed them yet */
static void cold(void) { mic_bank.regs[0x4B] = 0xFF; spk_bank.regs[0x0D] = 0xFC; }   /* after es7210_stop / es8311_suspend: what every close leaves */
static size_t n_mic(void) { return sizeof ES7210_STOP / sizeof ES7210_STOP[0]; }
static size_t n_spk(void) { return sizeof ES8311_SUSPEND / sizeof ES8311_SUSPEND[0]; }
/* the writes that were made, in order, are the mic table then the speaker table (or a prefix of one) */
static void check_tables(size_t mics, size_t spks) {
    assert((size_t)nwr == mics + spks);
    for (size_t i = 0; i < mics; i++) assert(wr[i].dev == 'M' && wr[i].reg == ES7210_STOP[i].reg && wr[i].val == ES7210_STOP[i].val);
    for (size_t i = 0; i < spks; i++) assert(wr[mics + i].dev == 'S' && wr[mics + i].reg == ES8311_SUSPEND[i].reg && wr[mics + i].val == ES8311_SUSPEND[i].val);
}
/* the expected event string: amp, mic probe + writes, speaker probe + writes, then the clocks */
static void expect(char *out, size_t mics, size_t spks) {
    strcpy(out, "a0aCa0rM"); for (size_t i = 0; i < mics; i++) strcat(out, "wM");
    strcat(out, "rS"); for (size_t i = 0; i < spks; i++) strcat(out, "wS");
    strcat(out, "R-T-");
}
#endif
int main(void) {
    char want[1024];
    // ---- the amplifier: the latch low, the pad an output, the latch low again; once per stop; never while a capture or a beep owns it ----
    reset();
#if S2_CODEC_OFF
    cold();
    assert(audio_capture_idle(true)); expect(want, 0, 0); assert(!strcmp(calls, want));
#else
    assert(audio_capture_idle(true) && !strcmp(calls, "a0aCa0R-T-"));
#endif
    assert(pa_level == 0 && !lock_held && s_idle);
    { char before[1024]; strcpy(before, calls);
      assert(audio_capture_idle(true) && !strcmp(calls, before));                      // a repeat is one stop
      assert(audio_capture_idle(false) && !strcmp(calls, before) && pa_level == 0); }  // "audio may start again" leaves the pin alone
    reset(); s_open = true;
    assert(!audio_capture_idle(true) && !calls[0] && pa_level == -1);
    reset(); atomic_store(&s_capture_requested, true);
    assert(!audio_capture_idle(true) && !calls[0] && pa_level == -1);
    reset(); lock_busy = true;
    assert(!audio_capture_idle(true) && !calls[0] && pa_level == -1 && !s_idle);
    reset(); s_codec_lock = NULL;                                                     // audio never came up: the pin is held low, once; no codec is touched
    assert(audio_capture_idle(true) && !strcmp(calls, "a0aCa0") && pa_level == 0);
    assert(audio_capture_idle(false) && !strcmp(calls, "a0aCa0"));
#if S2_CODEC_OFF
    // ---- the codecs ----
    // 1. fresh from boot (nothing has closed them): both are powered down while the clock still runs, then the clocks stop
    reset(); hot();
    assert(audio_capture_idle(true));
    expect(want, n_mic(), n_spk()); assert(!strcmp(calls, want)); check_tables(n_mic(), n_spk());
    assert(mic_bank.regs[0x4B] == 0xFF && spk_bank.regs[0x0D] == 0xFC);              // the registers the next probe reads: now "stopped"
    assert(mic_bank.regs[0x40] == 0xC0 && mic_bank.regs[0x01] == 0x7F && mic_bank.regs[0x06] == 0x07);   // es7210_stop's last values
    assert(spk_bank.regs[0x02] == 0x00 && spk_bank.regs[0x01] == 0x00 && spk_bank.regs[0x0E] == 0xFF && spk_bank.regs[0x12] == 0x02);
    assert(!lock_held && s_idle && !rx_chan.running && !tx_chan.running);
    // 2. S2 again with nothing used in between: the probe says stopped, nothing more is written
    assert(audio_capture_idle(false) && audio_capture_idle(true));
    assert(nwr == (int)(n_mic() + n_spk()));
    // 3. after a capture or a beep closed them (the driver left them stopped): two probe reads, no write
    reset(); cold();
    assert(audio_capture_idle(true) && nwr == 0); expect(want, 0, 0); assert(!strcmp(calls, want));
    // 4. one of the two still powered (a beep closed the speaker, the microphone has not been used; or the other way round)
    reset(); hot(); spk_bank.regs[0x0D] = 0xFC;
    assert(audio_capture_idle(true)); expect(want, n_mic(), 0); assert(!strcmp(calls, want)); check_tables(n_mic(), 0);
    reset(); hot(); mic_bank.regs[0x4B] = 0xFF;
    assert(audio_capture_idle(true)); expect(want, 0, n_spk()); assert(!strcmp(calls, want)); check_tables(0, n_spk());
    // 5. refused (capture open or requested, a beep holding the lock): not one probe, not one write
    reset(); hot(); s_open = true;
    assert(!audio_capture_idle(true) && !calls[0] && nwr == 0);
    reset(); hot(); atomic_store(&s_capture_requested, true);
    assert(!audio_capture_idle(true) && !calls[0] && nwr == 0);
    reset(); hot(); lock_busy = true;
    assert(!audio_capture_idle(true) && !calls[0] && nwr == 0 && !s_idle);
    //    ... and the retry after the beep (power_sleep.c retries on every render pass) then does the whole thing once
    lock_busy = false;
    assert(audio_capture_idle(true)); expect(want, n_mic(), n_spk()); assert(!strcmp(calls, want));
    // 6. a probe that cannot be read: that codec is left alone, the other one and the clocks are not held up, the call still succeeds
    reset(); hot(); mic_bank.fail_read = 1;
    assert(audio_capture_idle(true) && s_idle && !rx_chan.running && !tx_chan.running);
    check_tables(0, n_spk()); assert(!strncmp(calls, "a0aCa0rMrSwS", 12) && mic_bank.regs[0x4B] == 0x00);
    // 7. a write that fails: that codec's remaining writes are skipped (the next open writes everything again anyway), the rest goes on
    reset(); hot(); mic_bank.fail_write_at = 3;
    assert(audio_capture_idle(true) && s_idle && !rx_chan.running && !tx_chan.running);
    { int mic_done = 0, spk_done = 0;
      for (int i = 0; i < nwr; i++) { if (wr[i].dev == 'M') mic_done++; else spk_done++; }
      assert(mic_done == 2 && spk_done == (int)n_spk()); }
    // 8. no speaker (notify_init_failed left s_spk NULL): the microphone codec is still handled; neither: nothing to do but the amp
    reset(); hot(); s_spk = NULL;
    assert(audio_capture_idle(true)); assert(nwr == (int)n_mic() && wr[0].dev == 'M');
    reset(); hot(); s_mic = NULL; s_spk = NULL;
    assert(audio_capture_idle(true) && nwr == 0 && !strcmp(calls, "a0aCa0R-T-"));
    puts("audio S2 power-down (amp + codecs): amp low, codecs stopped with the clock still running, the driver's own sequence write for write, "
         "nothing touched while a capture or a beep owns the parts, probe and write faults do not hold up the sleep PASS");
#else
    // switch off: no probe, no write, nothing compiled
    reset(); mic_bank.regs[0x4B] = 0x00; spk_bank.regs[0x0D] = 0xFA;
    assert(audio_capture_idle(true) && !strcmp(calls, "a0aCa0R-T-") && nwr == 0);
    puts("audio S2 power-down (amp only, codec switch off): no codec register is read or written PASS");
#endif
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-audio-s2-') as folder:
    folder = Path(folder)
    (folder / 's2.c').write_text(code)
    for exe, defs in (('s2_both', ['-DS2_AMP_OFF=1', '-DS2_CODEC_OFF=1']), ('s2_amp', ['-DS2_AMP_OFF=1', '-DS2_CODEC_OFF=0'])):
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable', '-O1', '-g', *defs,
                        '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'), str(folder / 's2.c'),
                        '-o', str(folder / exe)], check=True)
        subprocess.run([str(folder / exe)], check=True)
    print('(' + driver_note + ')')

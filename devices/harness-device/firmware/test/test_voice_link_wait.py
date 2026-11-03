"""Voice right after the radio comes back (S2_WIFI_OFF): the first words are not lost.

The PTT can be pressed while the Wi-Fi radio is still starting (the PWR key starts it; a join and a handshake take seconds). The capture task starts at once and fills
the PCM ring; the sender used to send voice.begin at once, which fails without a session and aborted the turn. Now:
  * cable_client_wait_session() (cable_client.c, run here on the host against a fake clock) returns at once with a session up, returns false at once when no
    predicate is registered (USB mode, a radio that was never off) or the predicate says the radio is not coming back, and otherwise polls every 20 ms until a session
    is up, the predicate turns false, or max_ms have passed;
  * send_audio() (audio_client_habitat.c) calls it before audio_stream_begin(), with VOICE_LINK_WAIT_MS, which is shorter than the time the Wi-Fi ring holds:
    the ring is 98,304 B (BUFFER_BYTES_WIFI) of 16 kHz mono 16-bit PCM = 32,000 B/s = 3.072 s. Past the ring the capture aborts the turn as it always did
    (full-or-abort: "USB stalled beyond the PCM buffer; discarding utterance").
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

main = Path(__file__).resolve().parent / '../main'
client = (main / 'cable_client.c').read_text(encoding='utf-8').replace('\r\n', '\n')
habitat = (main / 'audio_client_habitat.c').read_text(encoding='utf-8').replace('\r\n', '\n')

fn = re.search(r'bool cable_client_wait_session\(uint32_t max_ms\)\n\{.*?^\}', client, re.M | re.S)
assert fn, 'cable_client_wait_session'
setter = re.search(r'static bool \(\*s_link_waking\)\(void\);\nvoid cable_client_set_link_waking\(bool \(\*waking\)\(void\)\) \{ s_link_waking = waking; \}', client)
assert setter

ring = int(re.search(r'#define BUFFER_BYTES_WIFI (\d+)', habitat)[1])
wait_ms = int(re.search(r'#define VOICE_LINK_WAIT_MS (\d+)', habitat)[1])
assert ring == 98304
ring_ms = ring * 1000 // (16000 * 2)
assert ring_ms == 3072 and wait_ms < ring_ms and wait_ms >= 2500, 'the wait is shorter than the ring, and long enough to matter'
send = re.search(r'static void send_audio\(void \*unused\)\n\{.*?^\}', habitat, re.M | re.S)[0]
assert send.index('cable_client_wait_session(VOICE_LINK_WAIT_MS)') < send.index('audio_stream_begin('), 'the wait comes before voice.begin'
assert send.index('xTaskNotifyGive(capture_task)') < send.index('cable_client_wait_session('), 'the capture is already running while the sender waits'

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define pdMS_TO_TICKS(x) (x)
static int64_t now_us, session_at_us, waking_until_us;
static volatile bool s_session;
static unsigned delays;
static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(unsigned ticks) { delays++; now_us += (int64_t)ticks * 1000; if (session_at_us && now_us >= session_at_us) s_session = true; }
static bool waking(void) { return now_us < waking_until_us; }
''' + setter[0] + '\n' + fn[0] + r'''
static void reset(void) { now_us = 1000000; session_at_us = waking_until_us = 0; s_session = false; delays = 0; s_link_waking = waking; }
int main(void) {
    reset(); s_session = true;                                   /* a session is up: no wait */
    assert(cable_client_wait_session(3000) && delays == 0);
    reset(); s_link_waking = 0;                                  /* no predicate (USB, or a radio that was never off): no wait, false */
    assert(!cable_client_wait_session(3000) && delays == 0);
    reset();                                                     /* the predicate says the radio is not coming back: no wait */
    assert(!cable_client_wait_session(3000) && delays == 0);
    reset(); waking_until_us = now_us + 60000000; session_at_us = now_us + 1500000;   /* the link comes after 1.5 s */
    int64_t t0 = now_us;
    assert(cable_client_wait_session(2800));
    assert(now_us - t0 >= 1500000 && now_us - t0 < 1500000 + 40000 && delays >= 70 && delays <= 80);   /* polled every 20 ms, left within one poll of the session */
    reset(); waking_until_us = now_us + 60000000;                /* the link never comes: the wait ends at max_ms, false */
    t0 = now_us;
    assert(!cable_client_wait_session(2800) && now_us - t0 >= 2800000 && now_us - t0 < 2800000 + 40000);
    reset(); waking_until_us = now_us + 1000000;                 /* the radio gives up (the predicate turns false) after 1 s: the wait ends then */
    t0 = now_us;
    assert(!cable_client_wait_session(2800) && now_us - t0 >= 1000000 && now_us - t0 < 1000000 + 40000);
    puts("voice link wait: no wait with a session or without a predicate, polls every 20 ms, ends at the session, the time or the predicate PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-voice-wait-') as folder:
    folder = Path(folder)
    (folder / 'w.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    str(folder / 'w.c'), '-o', str(folder / 'w')], check=True)
    subprocess.run([str(folder / 'w')], check=True)

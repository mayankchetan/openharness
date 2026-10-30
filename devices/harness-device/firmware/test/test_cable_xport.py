"""W5: cable_link.c over a fake transport (cable_xport.h): split reads, a partial write, a closed link.

The production reader_task, send_locked, cable_link_send, cable_link_host_present and cable_link_transport are
cut out of cable_link.c by name and compiled with the real decoder and a loopback transport. Also checks, from
source, that the USB driver is reached only from cable_xport_usb.c and that the vtable header documents the
wrapper (TLS) contract. The decoder and a wrapper transport are tested in test_cable_xport.c.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

main = Path(__file__).resolve().parent / '../main'
source = (main / 'cable_link.c').read_text()
usb = (main / 'cable_xport_usb.c').read_text()
header = (main / 'cable_xport.h').read_text()

# --- source rules ---------------------------------------------------------------------------------------
assert 'usb_serial_jtag' not in source and 'driver/usb_serial_jtag.h' not in source, 'cable_link.c is wire-agnostic'
for name in ('usb_serial_jtag_driver_install', 'usb_serial_jtag_driver_uninstall', 'usb_serial_jtag_read_bytes',
             'usb_serial_jtag_write_bytes', 'usb_serial_jtag_is_connected'):
    assert name in usb, name
assert re.search(r'#define USJ_RX_BUF \(32 \* 1024\)', usb) and re.search(r'#define USJ_TX_BUF 2048\b', usb)
assert '"cable_xport_usb.c"' in (main / 'CMakeLists.txt').read_text()
# the six operations of the contract
for field in ('open', 'close', 'read', 'write', 'is_up', 'kind'):
    assert re.search(r'\b' + field + r'\b', header[header.index('struct cable_xport {'):]), field
# the header documents how a TLS wrapper sits on it
for phrase in ('mbedtls_ssl_set_bio', 'MBEDTLS_ERR_SSL_WANT_WRITE', 'Single-owner rule for the ssl context',
               'SAME buffer', 'must not hold a lock that write() takes'):
    assert phrase in header, phrase

# --- the production functions over a fake transport ------------------------------------------------------
def function(name):
    match = re.search(r'^[^\n]*\b' + name + r'\([^;]*?\)\n\{.*?^\}', source.replace('\r\n', '\n'), re.M | re.S)
    assert match, name
    return match.group(0) + '\n'

defines = '\n'.join(re.findall(r'^#define (?:READ_CHUNK|READ_WAIT_MS|FRAME_IDLE_US|WRITE_WAIT_MS|LOG_WRITE_WAIT_MS) .+$',
                               source.replace('\r\n', '\n'), re.M))
code = r'''
#include "cable_link.h"
#include "power_sleep.h"   /* the reader asks power_sleep_wait_ms for its wait: READ_WAIT_MS with the flag off */
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
''' + defines + r'''
typedef struct { const uint8_t *bytes; size_t len; int ret; } read_t;
static read_t reads[2048];
static unsigned read_count, read_at, frames, ticks;
static int64_t now;
static jmp_buf done;
static uint8_t s_dec_buf[CABLE_MAX_FRAME];
static cable_decoder_buf_t s_decoder;
static const cable_xport_t *s_xport;
static cable_frame_cb s_cb;
static cable_tick_cb s_tick;
static void *s_ctx;
static atomic_bool s_running;
static int s_tx_lock = 1, lock_depth;
static uint8_t s_tx_frame[CABLE_MAX_FRAME];
static int64_t esp_timer_get_time(void) { return now; }
static int xSemaphoreTake(int lock, unsigned wait) { assert(lock == 1 && wait == portMAX_DELAY && !lock_depth); lock_depth++; return pdTRUE; }
static void xSemaphoreGive(int lock) { assert(lock == 1 && lock_depth == 1); lock_depth--; }

// loopback transport
static uint8_t wire[4 * CABLE_MAX_FRAME];
static size_t wire_len;
static unsigned writes, is_up_calls;
static int write_accepts;      /* >= 0: accept at most this many bytes; -1: accept all; -2: report a closed stream */
static bool up;
static int fake_read(const cable_xport_t *x, uint8_t *out, size_t cap, int wait) {
    assert(x == s_xport && cap == READ_CHUNK && wait == READ_WAIT_MS);
    if (read_at == read_count) longjmp(done, 1);
    const read_t r = reads[read_at++];
    if (r.ret < 0) return r.ret;
    assert(r.len <= cap);
    if (r.len) memcpy(out, r.bytes, r.len);
    return (int)r.len;
}
static int fake_write(const cable_xport_t *x, const uint8_t *p, size_t len, int wait) {
    assert(x == s_xport && wait == WRITE_WAIT_MS && lock_depth == 1);   /* under the tx lock, with the normal wait */
    writes++;
    if (write_accepts == -2) return -1;
    size_t n = write_accepts < 0 || (size_t)write_accepts > len ? len : (size_t)write_accepts;
    assert(wire_len + n <= sizeof wire);
    memcpy(wire + wire_len, p, n); wire_len += n;
    return (int)n;
}
static bool fake_is_up(const cable_xport_t *x) { assert(x == s_xport); is_up_calls++; return up; }
static const cable_xport_t fake_ble = { "fake", CABLE_XPORT_BLE, NULL, NULL, fake_read, fake_write, fake_is_up, NULL, NULL };
static const cable_xport_t fake_wifi = { "fake", CABLE_XPORT_WIFI, NULL, NULL, fake_read, fake_write, fake_is_up, NULL, NULL };

typedef struct { uint8_t type; size_t len; uint8_t first; } seen_t;
static seen_t seen[16];
static void collect(uint8_t version, uint8_t type, const uint8_t *p, size_t n, void *ctx) {
    assert(!ctx && version == CABLE_FRAME_VERSION);
    assert(frames < 16); seen[frames++] = (seen_t){ type, n, n ? p[0] : 0 };
}
static void tick(void *ctx) { assert(!ctx); ticks++; }
''' + function('reader_task') + function('send_locked') + function('cable_link_send') \
    + function('cable_link_host_present') + function('cable_link_transport') + r'''
static void reset(const cable_xport_t *x) {
    read_count = read_at = frames = ticks = writes = is_up_calls = 0; now = 0; wire_len = 0;
    write_accepts = -1; up = true; lock_depth = 0;
    s_xport = x; s_running = true;
    assert(cable_decoder_init_with(&s_decoder, s_dec_buf, sizeof s_dec_buf));
    s_cb = collect; s_tick = tick; s_ctx = NULL;
}
static void script(const uint8_t *p, size_t len, size_t step) {
    for (size_t i = 0; i < len; i += step) {
        assert(read_count < sizeof reads / sizeof reads[0]);
        reads[read_count++] = (read_t){ p + i, len - i < step ? len - i : step, 0 };
    }
}
static void run(void) { if (!setjmp(done)) reader_task(NULL); assert(ticks == read_count); }

static size_t put(uint8_t *out, uint8_t type, size_t n, uint8_t first) {
    uint8_t payload[CABLE_MAX_PAYLOAD];
    for (size_t i = 0; i < n; i++) payload[i] = (uint8_t)(0x20 + i % 11);
    if (n) payload[0] = first;
    int len = cable_frame_encode(type, payload, n, out, CABLE_MAX_FRAME);
    assert(len > 0); return (size_t)len;
}

int main(void) {
    static uint8_t stream[3 * CABLE_MAX_FRAME];
    size_t at = put(stream, CABLE_TYPE_JSON, 40, 1);
    at += put(stream + at, CABLE_TYPE_PCM, 1024, 2);
    at += put(stream + at, CABLE_TYPE_JSON, 3, 3);
    unsigned checks = 0;

    // Split reads: whatever the transport's chunking, the same three frames come out, in order.
    for (size_t step = 1; step <= READ_CHUNK; step = step * 3 + 1) {
        reset(&fake_ble); script(stream, at, step); run();
        assert(frames == 3 && seen[0].first == 1 && seen[1].type == CABLE_TYPE_PCM && seen[1].len == 1024 && seen[2].first == 3);
        assert(!s_decoder.len && !s_decoder.corrupt_frames && !s_decoder.discarded_bytes);
        checks++;
    }

    // A closed or failed stream: the reader keeps ticking (connection expiry runs on it), feeds nothing, and
    // the decoder is intact when bytes come back.
    reset(&fake_ble);
    for (int i = 0; i < 5; i++) reads[read_count++] = (read_t){ NULL, 0, -1 };
    reads[read_count++] = (read_t){ NULL, 0, 0 };
    script(stream, at, 100);
    run();
    assert(frames == 3 && ticks == read_count && !s_decoder.discarded_bytes);
    checks++;

    // Write: the whole encoded frame goes to the transport in one call, with the normal wait.
    reset(&fake_wifi);
    const uint8_t msg[] = "{\"t\":\"hello\"}";
    assert(cable_link_send(CABLE_TYPE_JSON, msg, sizeof msg - 1));
    uint8_t expect[CABLE_MAX_FRAME];
    int en = cable_frame_encode(CABLE_TYPE_JSON, msg, sizeof msg - 1, expect, sizeof expect);
    assert(writes == 1 && wire_len == (size_t)en && !memcmp(wire, expect, wire_len) && !lock_depth);
    checks++;

    // A partial write is "not sent" (a cut frame is the far decoder's to resync past), and the lock is released.
    reset(&fake_wifi); write_accepts = en / 2;
    assert(!cable_link_send(CABLE_TYPE_JSON, msg, sizeof msg - 1));
    assert(writes == 1 && wire_len == (size_t)(en / 2) && !lock_depth);
    write_accepts = 0;
    assert(!cable_link_send(CABLE_TYPE_JSON, msg, sizeof msg - 1) && writes == 2 && !lock_depth);
    write_accepts = -2;   /* closed stream */
    assert(!cable_link_send(CABLE_TYPE_JSON, msg, sizeof msg - 1) && writes == 3 && !lock_depth);
    // The link is still usable once the stream is back.
    write_accepts = -1; wire_len = 0;
    assert(cable_link_send(CABLE_TYPE_JSON, msg, sizeof msg - 1) && wire_len == (size_t)en);
    checks++;

    // An over-limit payload never reaches the transport.
    static uint8_t big[CABLE_MAX_PAYLOAD + 1];
    reset(&fake_wifi); writes = 0;
    assert(!cable_link_send(CABLE_TYPE_PCM, big, sizeof big) && writes == 0 && !lock_depth);
    // Not running: nothing is sent and the lock is never taken.
    s_running = false;
    assert(!cable_link_send(CABLE_TYPE_JSON, msg, sizeof msg - 1) && writes == 0);
    checks++;

    // is_up and kind come from the transport; before the link runs, host_present does not call it.
    reset(&fake_ble); up = true;  assert(cable_link_host_present() && is_up_calls == 1);
    up = false;                   assert(!cable_link_host_present() && is_up_calls == 2);
    assert(cable_link_transport() == CABLE_XPORT_BLE);
    reset(&fake_wifi);            assert(cable_link_transport() == CABLE_XPORT_WIFI);
    s_running = false; is_up_calls = 0;
    assert(!cable_link_host_present() && is_up_calls == 0);
    s_xport = NULL;               assert(cable_link_transport() == CABLE_XPORT_USB);
    checks++;

    printf("cable xport over cable_link: PASS (%u scenarios; split reads, closed stream, whole-frame write, partial write, oversize, is_up and kind)\n", checks);
}
'''
with tempfile.TemporaryDirectory(prefix='harness-cable-xport-') as folder:
    out = Path(folder)
    (out / 'xport.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-O1', '-g',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    '-I', str(main), str(out / 'xport.c'), str(main / 'cable_frame.c'), '-o', str(out / 'xport')], check=True)
    subprocess.run([str(out / 'xport')], check=True)

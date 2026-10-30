#include "cable_link.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "last_words.h"
#include "power_sleep.h"
#include "task_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cable";

// The USB driver FIFOs (USJ_RX_BUF, USJ_TX_BUF) and the rule that sizes them moved with the driver to
// cable_xport_usb.c. Nothing in this file depends on which wire is underneath (cable_xport.h).

// One read's worth of bytes off the transport. Small on purpose: the decoder is where reassembly happens, so
// this only has to keep the syscall rate sane, and every byte of it is internal RAM.
#define READ_CHUNK 256

// How long a read parks waiting for bytes. Not a poll interval — the driver wakes the task as soon as
// anything arrives. It only bounds how long the task sleeps with nothing to do.
#define READ_WAIT_MS 100

// A stopped host can leave a legal-length header followed by only half its
// payload. Magic inside the next welcome cannot escape that pending payload.
// Expire only an idle gap, not the total duration of a slowly fragmented frame.
#define FRAME_IDLE_US INT64_C(15000000)

// How long a write waits for the host to make room. Finite, and that is the point: an unplugged cable or
// an unopened port fills the TX FIFO and never drains it, and portMAX_DELAY there parks whatever task
// called send() forever. This device is unplugged or in front of a machine with no daemon most of the
// time, so "nobody is reading" has to be an ordinary, survivable answer.
#define WRITE_WAIT_MS 100

// A log line waits far less than a real message. Logging must never become back-pressure on the firmware:
// ESP_LOG is called from every task in the system, and at WRITE_WAIT_MS an unread port would stall each
// of them for a tenth of a second per line. A dropped log line costs a line; a stalled LVGL task costs
// the screen.
#define LOG_WRITE_WAIT_MS 5

// Reader task stack. The frame callback runs on this task, so it carries whatever the message layer
// eventually does — JSON parsing, LVGL updates, and during a firmware install esp_ota_write() plus a
// SHA-256 over the image read back out of flash.
//
// 6 KiB. Measured: the lowest free stack seen in a 20-minute run was 3,224 B ("ram: HWM ... cable_link=", with the OTA
// path built in, which has the deepest call chain). The failure this margin is against is real — the OTA path on this
// board's own earlier firmware overflowed an 8 KiB stack and shipped devices that could not update themselves.
//
// With the OTA path compiled out (DEVICE_NO_OTA, the product default) the reader's stack is in PSRAM (TASK_STACK_PSRAM_READER,
// task_util.h): the only flash call it reaches is an NVS read. With OTA built in it must stay internal, because esp_ota_begin
// asserts on a PSRAM stack.
#define READER_STACK 6144

// One log line's worth of formatted text. Anything longer is truncated rather than split: a log line is
// diagnostic, and half of one that arrives is worth more than a mechanism that could deadlock producing
// the other half.
#define LOG_LINE_MAX 512

// The decoder runs over a buffer this file owns (cable_decoder_init_with). Both are touched only by the reader
// task, and by cable_link_reset_decoder() from that task's callbacks (cable_link.h).
//
// 8.2 KB in PSRAM (EXT_RAM_BSS_ATTR, task W6), like s_tx_frame below. Safe because it is a CPU-only parse buffer:
// bytes reach it by memcpy from the reader's 256-byte chunk (cable_decoder_buf_feed), never from a peripheral, and
// the frame callback only parses it, copies it, or passes it to esp_ota_write, which copies a non-DRAM source
// through a stack buffer (esp_flash_write) instead of using DMA. test_decoder_psram.py pins these facts. The USB
// driver's own RX ring (32 KB, cable_xport_usb.c) stays internal: the ISR writes it.
static EXT_RAM_BSS_ATTR uint8_t s_dec_buf[CABLE_MAX_FRAME];
static cable_decoder_buf_t s_decoder;
static const cable_xport_t *s_xport;
static cable_frame_cb  s_cb;
static cable_tick_cb   s_tick;
static void           *s_ctx;
static atomic_bool     s_running;

// Serialises the shared encode buffer AND the write, so two tasks sending at once cannot interleave
// halves of two frames onto the wire. A frame split down the middle by a second sender is not something
// the far end can resync out of — both halves have valid magic and neither has a valid CRC.
static SemaphoreHandle_t s_tx_lock;

// 8.2 KB of BSS rather than a stack array (it would not fit) or a malloc (a failed allocation mid-session
// on a microcontroller is a worse outcome than a known, always-paid 8 KB).
//
// Full-size on purpose, even though nothing sends anything close: at CABLE_MAX_FRAME the encoder can
// never fail for lack of room here, so a -1 from cable_frame_encode means exactly one thing — the payload
// is over the protocol's own limit — instead of two things that need telling apart.
// In PSRAM (EXT_RAM_BSS_ATTR): the transport's write() copies what it keeps before it returns (the contract in
// cable_xport.h; the USB driver copies into its ring buffer from task context), so no peripheral ever reads this
// array and the DMA/cache-off rules do not apply.
static EXT_RAM_BSS_ATTR uint8_t s_tx_frame[CABLE_MAX_FRAME];

// ── log framing ─────────────────────────────────────────────────────────────────────────────────────

// Hooks can still be executing on the other core when the session ends. Keep
// the previous function valid for their whole lifetime; never clear it to NULL.
static _Atomic(vprintf_like_t) s_prev_vprintf = vprintf;
static atomic_bool s_log_framing;
static atomic_uint s_dropped_logs;

static bool send_locked(uint8_t type, const uint8_t *payload, size_t payload_len, int wait_ms)
{
    int len = cable_frame_encode(type, payload, payload_len, s_tx_frame, sizeof(s_tx_frame));
    if (len < 0) return false;
    return s_xport->write(s_xport, s_tx_frame, (size_t)len, wait_ms) == len;
}

static int log_vprintf(const char *fmt, va_list args)
{
    // ESP_EARLY_LOG/panic output bypasses this hook. Ordinary logs from an ISR
    // cannot take a mutex or safely fall back to the libc console.
    if (xPortInIsrContext()) return 0;
    if (!atomic_load(&s_log_framing)) return atomic_load(&s_prev_vprintf)(fmt, args);

    static char line[LOG_LINE_MAX];   // guarded by s_tx_lock, like s_tx_frame
    if (!s_running || xSemaphoreGetMutexHolder(s_tx_lock) == xTaskGetCurrentTaskHandle() ||
        xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(LOG_WRITE_WAIT_MS)) != pdTRUE) {
        // Recursion or congestion costs one diagnostic, never a raw console
        // write mixed into a protocol frame or an unbounded wait by the caller.
        atomic_fetch_add_explicit(&s_dropped_logs, 1, memory_order_relaxed);
        return 0;
    }
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 1;
        // Trailing newline is the console's business, not the protocol's: the frame IS the line.
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
        // Into the RTC ring first — that copy survives the reboot the next line may be the last before.
        if (len > 0) last_words_add(line, len);
        if (len > 0 && !send_locked(CABLE_TYPE_LOG, (const uint8_t *)line, len, LOG_WRITE_WAIT_MS))
            atomic_fetch_add_explicit(&s_dropped_logs, 1, memory_order_relaxed);
    }
    xSemaphoreGive(s_tx_lock);
    return n;
}

void cable_link_set_log_framing(bool on)
{
    if (!s_running) return;
    if (on) {
        if (!atomic_load(&s_log_framing)) {
            atomic_store(&s_prev_vprintf, esp_log_set_vprintf(log_vprintf));
            atomic_store(&s_log_framing, true);
        }
        return;
    }
    // Restore the plain console FIRST, then say so — so the line explaining what happened is written the
    // way whoever is now watching the port with a serial monitor can read it.
    if (atomic_exchange(&s_log_framing, false)) {
        esp_log_set_vprintf(atomic_load(&s_prev_vprintf));
    }
}

// ── link ────────────────────────────────────────────────────────────────────────────────────────────

static void reader_task(void *arg)
{
    (void)arg;
    uint8_t chunk[READ_CHUNK];
    int64_t last_rx_us = 0;
    while (1) {
        // READ_WAIT_MS; 2 s while the chip may sleep (no host can be talking then: a host would have kept it awake)
        // n < 0 (closed or failed stream) is handled like n == 0: nothing to feed, and the tick still runs, so
        // connection expiry keeps working. A transport that fails must still wait out its wait (cable_xport.h).
        int n = s_xport->read(s_xport, chunk, sizeof(chunk), power_sleep_wait_ms(PP_POLLER_CABLE_READER, READ_WAIT_MS));
        const int64_t now = esp_timer_get_time();
        // This task owns the decoder. Reset before feeding a newly arrived
        // read too: scheduling need not provide an intervening empty read.
        if (s_decoder.len && now - last_rx_us >= FRAME_IDLE_US)
            cable_decoder_buf_reset(&s_decoder);
        // Never fails and never rejects: everything arriving here is untrusted, starts mid-stream after
        // every boot, and the only useful response to a byte that makes no sense is to step over it.
        if (n > 0) {
            cable_decoder_buf_feed(&s_decoder, chunk, (size_t)n, s_cb, s_ctx);
            last_rx_us = now;
        }
        if (s_tick) s_tick(s_ctx);
    }
}

bool cable_link_start_with(const cable_xport_t *xport, cable_frame_cb cb, cable_tick_cb tick, void *ctx)
{
    if (s_running) return true;
    if (!xport) return false;

    s_xport = xport;
    s_cb = cb;
    s_tick = tick;
    s_ctx = ctx;
    if (!cable_decoder_init_with(&s_decoder, s_dec_buf, sizeof s_dec_buf)) return false;

    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) {
        ESP_LOGE(TAG, "no memory for the tx lock — link disabled");
        return false;
    }

    if (!xport->open(xport)) {
        // The transport has already said why (it knows its own driver). What it costs is the same on any wire:
        // the dial comes up, draws its screen, and simply never hears from the machine.
        ESP_LOGE(TAG, "%s transport did not open — no link to the daemon", xport->name);
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
        return false;
    }

    // The new task may receive a welcome before xTaskCreate returns. Make the
    // installed driver/send path visible before that callback can run.
    s_running = true;
#if TASK_STACK_PSRAM_READER
    const BaseType_t reader_ok = task_create_psram(reader_task, "cable_link", READER_STACK, NULL, 5, NULL, tskNO_AFFINITY);
#else
    const BaseType_t reader_ok = xTaskCreate(reader_task, "cable_link", READER_STACK, NULL, 5, NULL);
#endif
    if (reader_ok != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "reader task create failed — no link to the daemon");
        xport->close(xport);
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
        return false;
    }

    ESP_LOGI(TAG, "%s link up (frame v%d, max frame %d B)", xport->name,
             CABLE_FRAME_VERSION, CABLE_MAX_FRAME);
    return true;
}

bool cable_link_start(cable_frame_cb cb, cable_tick_cb tick, void *ctx)
{
    return cable_link_start_with(cable_xport_usb(), cb, tick, ctx);
}

bool cable_link_send(uint8_t type, const uint8_t *payload, size_t payload_len)
{
    if (!s_running) return false;

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    const bool ok = send_locked(type, payload, payload_len, WRITE_WAIT_MS);
    const bool too_big = payload_len > CABLE_MAX_PAYLOAD;
    xSemaphoreGive(s_tx_lock);

    if (ok) return true;
    if (too_big) {
        // A bug on this side, and one the far end could only ever report back as noise, so it has to be
        // caught and named here.
        ESP_LOGE(TAG, "refusing to send %u-byte payload (max %d)", (unsigned)payload_len, CABLE_MAX_PAYLOAD);
        return false;
    }
    // DEBUG, not WARN. "Nobody is draining the port" is this device's resting state, not a fault: it is
    // unplugged, or plugged into a machine with no daemon. At WARN the log would be a wall of identical
    // lines whenever nothing is wrong, which is how a log stops being read at all.
    ESP_LOGD(TAG, "short write (host not reading)");
    return false;
}

bool cable_link_host_present(void)
{
    // Guarded on s_running because the transport is undefined before open, and "no transport" is
    // indistinguishable from "no host" to everything above: both mean nothing can be said on this port.
    return s_running && s_xport->is_up(s_xport);
}

void cable_link_counters(uint32_t *corrupt_frames, uint32_t *discarded_bytes)
{
    if (corrupt_frames) *corrupt_frames = s_decoder.corrupt_frames;
    if (discarded_bytes) *discarded_bytes = s_decoder.discarded_bytes;
}

uint32_t cable_link_dropped_logs(void) { return atomic_load(&s_dropped_logs); }

cable_xport_kind_t cable_link_transport(void)
{
    return s_xport ? s_xport->kind : CABLE_XPORT_USB;
}

void cable_link_reset_decoder(void)
{
    cable_decoder_buf_reset(&s_decoder);
}

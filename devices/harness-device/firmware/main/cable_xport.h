// The byte-stream transport under cable_link (layer L0 of docs/specs/cable-wifi-link.md section 0).
//
// cable_link owns the parts that do not depend on the wire: the frame decoder, the tx lock, log framing,
// the reader task and the counters. A transport owns only "bytes in, bytes out". Today there is one,
// USB-Serial-JTAG (cable_xport_usb.c). The same vtable is meant to carry TCP (Wi-Fi mode) and a BLE L2CAP
// CoC (phone bridge) later, and a TLS wrapper that sits on top of any of them. Nothing here knows TLS,
// Wi-Fi or BLE; the contract below is written so those fit without a change to this header.
//
// ── THE CONTRACT ────────────────────────────────────────────────────────────────────────────────────
// Every call takes the transport itself as its first argument (const: the descriptor of a plain
// transport lives in flash). `ctx` is the transport's private, mutable state.
//
//   open(x)        Bring the transport up. Returns false on failure and leaves nothing allocated. Called
//                  once, from cable_link_start, before the reader task exists.
//   close(x)       Undo open. Safe after a failed or partial open. Called when start fails after open.
//   read(x, buf, cap, wait_ms)
//                  Wait at most wait_ms (0 = poll, never block) for bytes and copy up to cap of them into
//                  buf. Returns the byte count (> 0), 0 when nothing arrived in time, or < 0 when the
//                  stream is closed or failed. A short read is normal. Called ONLY from the reader task.
//                  It must not hold a lock that write() takes while it waits: a reader parked inside
//                  read() would otherwise starve every sender. `buf` is on the reader task's stack, which
//                  is in PSRAM in the product build: fill it with the CPU, never by DMA.
//   write(x, buf, len, wait_ms)
//                  Hand len bytes to the stream, waiting at most wait_ms for room. Returns the number of
//                  bytes accepted (0 = none within the wait), or < 0 when the stream is closed or
//                  failed. cable_link calls it with the whole encoded frame, holding its tx lock, so two
//                  writes never overlap; a write may overlap a read. `buf` is cable_link's frame buffer,
//                  which sits in PSRAM and is reused as soon as the call returns: a transport copies what
//                  it keeps and never hands `buf` to DMA or keeps the pointer.
//                    * A transport whose far end can resync (USB: magic scan plus CRC) may return a
//                      partial count. cable_link treats any count other than len as "not sent".
//                    * A transport whose stream CANNOT resync (anything under or inside TLS) must return
//                      0 (nothing taken), len (all taken), or < 0, and close itself rather than leave a
//                      half record behind. Once the first byte of a call is accepted it finishes the
//                      call, up to a hard cap of its own (2 s), or closes (finding F3 of the Wi-Fi spec).
//                      If a write returns 0, the caller may retry with the SAME buffer and length; a
//                      TLS wrapper relies on that (mbedTLS requires the same buffer after "want write").
//   is_up(x)       The stream is connected right now (USB: a host is driving the port; TCP: the socket
//                  is established; BLE: the channel is open; TLS: the handshake finished).
//   kind           Which wire this is. Read-only. A wrapper reports the kind of its inner transport, so
//                  the layers above (refuse firmware over radio, label the status) see usb | wifi | ble.
//   wait_readable(x, wait_ms)   OPTIONAL (NULL = absent; keep it the LAST member so a positional initializer need not name it).
//                  Wait at most wait_ms for the stream to become readable and consume nothing. Returns > 0 when a
//                  read() would not block (bytes, an end of stream or an error is pending), 0 on timeout, < 0 when
//                  the stream is closed or failed. It changes no state of the transport, and unlike read() and write()
//                  it may run while another task is inside them: a wrapper uses it to wait WITHOUT holding its lock
//                  (a reader parked in a 2 s wait then costs one wake, not one per poll slice). A close from another
//                  task may not wake it at once, so a caller bounds each wait (cable_xport_tls.c: 500 ms) and checks
//                  its own state between waits.
//
// ── WRAPPING: A TLS TRANSPORT OVER ANY INNER TRANSPORT (phone-bridge task B2) ───────────────────────
// A wrapper is a cable_xport_t whose `inner` points at another cable_xport_t. It implements the same six
// operations on top of the inner ones, so cable_link cannot tell it from a plain transport. For
// cable_xport_tls.c the inner calls are the mbedTLS BIO callbacks:
//
//     mbedtls_ssl_set_bio(&ssl, inner, bio_send, NULL, bio_recv_timeout);
//     bio_send(inner, buf, len)               -> n = inner->write(inner, buf, len, 0);
//                                                n > 0: return n.  n == 0: MBEDTLS_ERR_SSL_WANT_WRITE.
//                                                n < 0: MBEDTLS_ERR_NET_SEND_FAILED.
//     bio_recv_timeout(inner, buf, len, ms)   -> n = inner->read(inner, buf, len, ms);
//                                                n > 0: return n.  n == 0: MBEDTLS_ERR_SSL_TIMEOUT
//                                                (WANT_READ when ms == 0).  n < 0: MBEDTLS_ERR_NET_CONN_RESET.
//
// That is why read() takes a wait and may return short, and why write() returns a count and may be
// retried with the same buffer: those are the two things the BIO callbacks need. Inner write() may
// return a partial count, because the BIO contract allows it; the wrapper's own write() then loops until
// the whole frame is taken or its cap runs out (and closes), so cable_link above it still sees the
// all-or-nothing rule.
//
// Single-owner rule for the ssl context. One mbedtls_ssl_context must never run in two tasks at once.
// cable_link calls read() from the reader task and write() from whichever task sends, so a wrapper has
// two valid designs and must pick one:
//   (1) one I/O task owns the context: every ssl_read and ssl_write runs there, and the wrapper's
//       write() only queues a frame and waits for that task to finish it (the link task of the
//       Wi-Fi and phone-bridge specs); or
//   (2) one mutex around every ssl_read, ssl_write and handshake step. A wrapper read(wait_ms) then
//       polls the inner transport in slices (inner->read with a short or zero wait) and drops the mutex
//       between slices, so a waiting reader never blocks a writer.
// Either way the wrapper is the ONLY caller of the inner transport's read, write and close. The inner
// transport is single-owner by construction: its read() and write() are driven by the wrapper's lock
// or task, not by cable_link. Neither design needs a change to this header.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CABLE_XPORT_USB = 0,
    CABLE_XPORT_WIFI,
    CABLE_XPORT_BLE,
} cable_xport_kind_t;

static inline const char *cable_xport_kind_name(cable_xport_kind_t kind)
{
    switch (kind) {
    case CABLE_XPORT_USB:  return "usb";
    case CABLE_XPORT_WIFI: return "wifi";
    case CABLE_XPORT_BLE:  return "ble";
    }
    return "?";
}

typedef struct cable_xport cable_xport_t;

struct cable_xport {
    const char         *name;       // for log lines: "usb", "tcp", "tls"
    cable_xport_kind_t  kind;       // which wire; a wrapper copies its inner transport's kind
    bool (*open)(const cable_xport_t *x);
    void (*close)(const cable_xport_t *x);
    int  (*read)(const cable_xport_t *x, uint8_t *buf, size_t cap, int wait_ms);
    int  (*write)(const cable_xport_t *x, const uint8_t *buf, size_t len, int wait_ms);
    bool (*is_up)(const cable_xport_t *x);
    void               *ctx;        // the transport's private state
    const cable_xport_t *inner;     // wrapper transports only (TLS over TCP or CoC); NULL for a plain one
    int  (*wait_readable)(const cable_xport_t *x, int wait_ms);   // optional, see above; LAST member on purpose
};

// The USB-Serial-JTAG transport (cable_xport_usb.c). One static instance.
const cable_xport_t *cable_xport_usb(void);

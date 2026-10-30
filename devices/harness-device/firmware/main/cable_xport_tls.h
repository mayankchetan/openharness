// TLS 1.2 PSK over any cable transport (task B2 of the phone-bridge spec; replaces the esp-tls design of Wi-Fi task W7).
//
// cable_xport_tls_wrap() takes an inner transport (TCP, a BLE L2CAP channel, anything with the cable_xport.h contract)
// and returns a transport of the same shape whose bytes are TLS records. The inner read and write are the mbedTLS BIO
// callbacks, which is why esp-tls is not used: its BIO is bound to a socket fd. No crypto of our own lives here.
//
//   Version   TLS 1.2 only (min and max are both pinned).
//   Suites    TLS-ECDHE-PSK-WITH-AES-128-CBC-SHA256 (0xC037), then -AES-256-CBC-SHA384 (0xC038). Nothing else is offered:
//             no plain PSK (no forward secrecy), no certificate suite. mbedTLS has no ECDHE-PSK GCM suite (W0 report).
//   Identity  the dial MAC string in upper case ("B8:1F:3F:CA:E5:58"); the PSK is 32 bytes. The wrapper copies both into
//             the mbedTLS config and keeps no pointer to the caller's buffers.
//
// The dial is always the TLS client. The handshake runs inside open(), after the inner transport is open and up, with a
// 5 s deadline; on any failure open() closes the inner transport and returns false.
//
// Concurrency. One mbedtls_ssl_context is never used by two tasks at once: one mutex covers every ssl_read, ssl_write and
// handshake step. read() waits for data in slices of 10 ms and drops the mutex between slices, so a waiting reader never
// starves a writer (and a reader yields when a writer is queued). The inner transport is called only from under that mutex.
//
// Write rule (finding F3 of the Wi-Fi spec). write() takes the whole buffer or the link dies. mbedTLS copies the record
// into its own buffer on the first ssl_write call and then needs the SAME arguments on every retry, so once that call has
// run the wrapper retries with the same buffer until the record is out, for at most 2 s, then closes the link and returns
// -1. While it waits for room the mutex is free, so read() keeps running: a stalled send never stops the receive side
// (a peer may be waiting for us to read before it reads). The caller's wait_ms bounds only the wait for the mutex. write()
// returns 0 (nothing taken, the link is fine) when the mutex stays busy, or when another write() is still finishing its
// record; cable_link's tx lock means that second case does not arise in the product.
//
// Logging. Error paths log mbedTLS error codes only. The key is never logged, anywhere.
#pragma once

#include "cable_xport.h"

#define CABLE_XPORT_TLS_PSK_MAX 32   // MBEDTLS_PSK_MAX_LEN for a 256-bit key; the product key is exactly this long

// Build a TLS wrapper over `inner` (not owned; it must outlive the wrapper). `identity` is NUL-terminated. Returns NULL
// on bad arguments or when memory runs out. The returned transport reports the inner transport's kind and its name is "tls".
const cable_xport_t *cable_xport_tls_wrap(const cable_xport_t *inner, const char *identity,
                                          const uint8_t *psk, size_t psk_len);

// Close the wrapper (and with it the inner transport) and free everything, zeroizing the key copy. NULL is allowed.
void cable_xport_tls_free(const cable_xport_t *x);

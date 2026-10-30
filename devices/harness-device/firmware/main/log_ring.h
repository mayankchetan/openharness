// A byte ring of log lines that drops its OLDEST lines when full and never waits for its reader.
//
// Wi-Fi mode (cable-wifi-link.md finding F2): the log hook (cable_link.c) runs in every task, including the tcpip and Wi-Fi tasks, and over a
// socket or a TLS record a log line sent from there can wait on the task that is itself logging. So in Wi-Fi mode the hook formats the line
// and puts it here, and the net task (net_link.c) sends what it finds. The hook never calls a socket, lwIP or mbedTLS.
//
// One entry is a 16-bit length and the line (at most LOG_RING_LINE_MAX bytes; a longer line is cut). Many producers, ONE consumer.
//
// The lock. Producers and the consumer share one short lock around a copy of at most 514 bytes and a few index updates. It is never held
// across I/O, a wait or a call out of this file. On the target it is a portMUX critical section (a copy to PSRAM is a few microseconds);
// on the host it is a mutex. "Never blocks" means: no push waits for the consumer or for the network, whatever state either is in.
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOG_RING_LINE_MAX 512

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t tail;   // offset of the oldest entry
    uint32_t used;   // bytes in use, headers included
    atomic_uint overwritten;   // lines dropped to make room
    atomic_uint rejected;      // lines that could not fit in an empty ring, or arrived with a bad argument
} log_ring_t;

// `storage` is owned by the caller and must stay valid. A ring of fewer than 3 bytes accepts nothing.
void log_ring_init(log_ring_t *r, uint8_t *storage, uint32_t cap);

// Queue one line. Returns false (and counts it as rejected) only for an empty line or one that cannot fit in an empty ring; a full ring drops
// its oldest lines instead and still returns true.
bool log_ring_push(log_ring_t *r, const uint8_t *line, size_t len);

// Take the oldest line into `out` (consumer only). Returns its length, or 0 when the ring is empty or `out_cap` is too small for the line
// (which then stays queued). Pass a buffer of LOG_RING_LINE_MAX.
size_t log_ring_pop(log_ring_t *r, uint8_t *out, size_t out_cap);

uint32_t log_ring_used(log_ring_t *r);
uint32_t log_ring_overwritten(const log_ring_t *r);
uint32_t log_ring_rejected(const log_ring_t *r);

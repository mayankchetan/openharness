// A byte ring of log lines that drops its OLDEST lines when full and never waits for its reader.
//
// Wi-Fi mode (cable-wifi-link.md finding F2): the log hook (cable_link.c) runs in every task, including the tcpip and Wi-Fi tasks, and over a
// socket or TLS record a log line sent from there can wait on the task that is itself logging. So in Wi-Fi mode the hook formats the line
// and puts it here, and the net task (net_link.c) sends what it finds. The hook never calls a socket, lwIP or mbedTLS.
//
// One entry is a 3-byte header (16-bit length, one state byte) and the line (at most LOG_RING_LINE_MAX bytes; a longer line is cut).
// Many producers, ONE consumer.
//
// THE LOCK COVERS INDEXES ONLY. A push is reserve, copy, commit: the short lock (a portMUX critical section on the target, a mutex on the host)
// is taken to make room (dropping the oldest COMMITTED entries by moving the tail), to write the header and move the head, and again to flip the
// state byte to READY. The copy of the line itself, up to 512 bytes into PSRAM, happens between the two, outside the lock, into a slot that
// belongs to that producer alone. A pop mirrors it: claim the oldest READY entry under the lock, copy it out unlocked, release it under the lock.
// So interrupts are masked for a handful of loads and stores, never for a memcpy.
//
// What that costs, and why it is acceptable: an entry that is being written (WRITING) or read (CLAIMED) cannot be recycled. If the oldest
// entry is in that state when a producer needs room, the producer's line is refused and counted (rejected) instead of overwriting memory
// somebody is using. That happens only when the ring is full AND a copy is in flight, a few microseconds wide: a producer's own copy, or the
// consumer's copy of the oldest line. In that window the NEWEST line is lost instead of the oldest; it is counted either way
// (cable_link_dropped_logs() adds both). Under a flood the host test sees many of these (the producers there outrun the consumer by far); in the
// product the ring is rarely full while the consumer is draining. Nothing ever waits.
// A producer preempted between reserve and commit holds its slot until it runs again; the entries behind it are not readable until it
// commits, and the consumer sees an empty ring meanwhile.
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOG_RING_LINE_MAX 512
#define LOG_RING_HDR 3   // length (2 bytes, little endian) and the state byte

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t tail;   // offset of the oldest entry
    uint32_t used;   // bytes in use, headers included
    atomic_uint overwritten;   // lines dropped to make room
    atomic_uint rejected;      // lines that could not be taken: too big for an empty ring, no room behind an entry in flight, or a bad argument
} log_ring_t;

// A reserved or claimed entry: where its header is and how long the line is. Valid until commit or release.
typedef struct { uint32_t at; uint32_t len; } log_ring_slot_t;

// `storage` is owned by the caller and must stay valid. A ring too small for a one-byte line (LOG_RING_HDR + 1) accepts nothing.
void log_ring_init(log_ring_t *r, uint8_t *storage, uint32_t cap);

// Queue one line. Returns false (and counts it as rejected) for an empty line, one that cannot fit in an empty ring, or one that finds the oldest
// entry in flight; a full ring otherwise drops its oldest lines instead and still returns true. Reserve, copy, commit.
bool log_ring_push(log_ring_t *r, const uint8_t *line, size_t len);

// Take the oldest line into `out` (consumer only). Returns its length, or 0 when nothing is readable or `out_cap` is too small for the line
// (which then stays queued). Pass a buffer of LOG_RING_LINE_MAX. Claim, copy, release.
size_t log_ring_pop(log_ring_t *r, uint8_t *out, size_t out_cap);

// The two halves of a push and of a pop, exposed so a test can hold an entry in flight. `len` is 1..LOG_RING_LINE_MAX.
bool log_ring_reserve(log_ring_t *r, size_t len, log_ring_slot_t *slot);
void log_ring_write(log_ring_t *r, const log_ring_slot_t *slot, const uint8_t *line);   // copies slot->len bytes; no lock
void log_ring_commit(log_ring_t *r, const log_ring_slot_t *slot);
bool log_ring_claim(log_ring_t *r, size_t out_cap, log_ring_slot_t *slot);
void log_ring_read(log_ring_t *r, const log_ring_slot_t *slot, uint8_t *out);           // copies slot->len bytes; no lock
void log_ring_release(log_ring_t *r, const log_ring_slot_t *slot);

uint32_t log_ring_used(log_ring_t *r);
uint32_t log_ring_overwritten(const log_ring_t *r);
uint32_t log_ring_rejected(const log_ring_t *r);

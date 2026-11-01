// The BLE transport of the phone mode (task B4b): a byte stream over one LE credit-based L2CAP channel (a CoC), behind the cable_xport.h contract.
// docs/specs/cable-ble-link.md has the wire; this file is the part of the dial that turns SDUs into a stream and a stream into SDUs.
//
// It knows nothing of NimBLE. ble_link.c owns the stack and gives this module a PORT (four function pointers) and tells it what happens on the air
// (channel up and down, an SDU arrived, the stack unstalled the channel). That split is what lets test/test_cable_xport_ble.c run all of the logic on
// the host with a fake port.
//
//   RECEIVE. NimBLE hands one SDU per event, and an SDU may be up to our MTU (4,032 B). on_sdu() copies it into an 8 KB ring and wakes the reader.
//   The ring has a CREDIT RULE: the stack gets a new receive buffer (port.rearm, which is what puts credits back on the air) only while a WHOLE SDU
//   still fits in the ring. So the SDU that is in flight always fits, nothing is ever dropped for lack of room, and a reader that falls behind
//   stops the phone by credits instead of losing bytes. If a reader does not drain, no rearm happens, and the phone's pump (relay rule 3) stalls
//   and closes after 4 s. An SDU that does not fit anyway (a peer that sends past its credits) breaks the stream: the channel is closed.
//
//   SEND. write() is all-or-nothing, the rule of a transport that cannot resync (cable_xport.h): once the first byte is taken the call finishes, or
//   closes the channel, within CABLE_XPORT_BLE_WRITE_CAP_MS (2 s, `write_cap_ms` of the vector file). The buffer is split into SDUs of at most the
//   peer's SDU MTU (and 4,032). The stack's answers (cable_xport_ble_send_t):
//     OK       the SDU was taken.
//     STALLED  taken, but the credits ran out: wait for the TX_UNSTALLED event (on_tx_unstalled) before the next SDU.
//     BUSY     the stack still holds the previous SDU and took nothing: wait a moment and offer the same SDU again.
//     NOMEM    no buffer for it: nothing was taken; try again in a tick.
//     ERR      the channel is gone or unusable: the stream is dead.
//   Nothing taken in the whole cap (BUSY or NOMEM throughout) is "0, the link is fine": the caller may retry with the same buffer. Anything taken
//   and then stuck is "-1 and the channel is closed". The caller's wait_ms is not used: there is no lock to wait for (one writer at a time; a second
//   concurrent write() returns 0).
//
// Threads. on_sdu, on_tx_unstalled, channel_up and channel_down come from the NimBLE host task; read comes from the TLS wrapper's reader and write
// from the sender, as in cable_xport.h. One mutex covers the ring and its flags; two binary semaphores carry "data or a state change" and "unstalled".
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cable_xport.h"

#define CABLE_XPORT_BLE_SDU_MAX 4032u   // our SDU MTU (spec: coc_mtu_bytes); also the longest SDU we send
#define CABLE_XPORT_BLE_RING    8192u   // two SDUs
#ifndef CABLE_XPORT_BLE_WRITE_CAP_MS
#define CABLE_XPORT_BLE_WRITE_CAP_MS 2000   // spec: write_cap_ms. The host test builds it shorter; test_ble_link_source.py pins this default.
#endif

typedef enum {
    BLE_SEND_OK = 0,
    BLE_SEND_STALLED,
    BLE_SEND_BUSY,
    BLE_SEND_NOMEM,
    BLE_SEND_ERR,
} cable_xport_ble_send_t;

typedef struct {
    void *ctx;
    // One SDU, 1..peer_mtu bytes. The stack copies what it keeps: `data` is reused as soon as this returns.
    cable_xport_ble_send_t (*send)(void *ctx, const uint8_t *data, size_t len);
    // Give the stack a receive buffer for the next SDU. Safe from any task.
    void (*rearm)(void *ctx);
    // Close the channel (the ACL link and the bond stay). Safe from any task; the stack answers with COC_DISCONNECTED.
    void (*disconnect)(void *ctx);
    // The peer's SDU MTU; 0 if it cannot be read (the transport then sends 23-byte SDUs, the smallest an L2CAP channel can have).
    size_t (*peer_mtu)(void *ctx);
} cable_xport_ble_port_t;

// The one transport. Its name is "ble", its kind CABLE_XPORT_BLE. Until init() it is never up and every read and write says -1.
const cable_xport_t *cable_xport_ble(void);

// Allocate the ring and the primitives (ring in PSRAM when it can be, internal otherwise) and keep the port. Idempotent. False when memory runs out
// or the port is incomplete.
bool cable_xport_ble_init(const cable_xport_ble_port_t *port);
// Free everything. The caller has stopped every task that calls read() or write() (the TLS wrapper's reader and sender) first.
void cable_xport_ble_deinit(void);

// What happens on the air, from the host task.
void cable_xport_ble_channel_up(void);                          // the channel opened: a clean ring, up
void cable_xport_ble_channel_down(void);                        // the channel closed (either side): readers and writers are woken; unread bytes stay readable
bool cable_xport_ble_on_sdu(const uint8_t *data, size_t len);   // an SDU arrived. False: it did not fit (the stream is broken and the channel is being closed)
void cable_xport_ble_on_tx_unstalled(void);                     // the stack has credits again

// open(): true exactly when the channel is up (ble_link opens the TLS wrapper over this only after the channel opened).
// close(): ask the stack to close the channel, once, and be down at once.

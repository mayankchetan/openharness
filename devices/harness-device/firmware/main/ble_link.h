// Phone mode: the BLE link to the phone bridge app, and through it to the daemon (docs/specs/cable-ble-link.md; task B4b of the phone bridge).
//
//   app_main.c:   const bool ble = ble_link_start();
//                 cable_client_start_with(ble ? ble_link_xport() : ...);
//
// ble_link_start() is the one switch. It reads NVS (config_ble_load); unless the phone settings are whole it returns false having done nothing else: no
// radio call, no NimBLE object, no task. A default image (built without NimBLE: DEVICE_BLE_LINK is not defined) has the inline stubs below and carries no
// BLE code at all.
//
// What runs. A short-lived internal-stack task (ble_init) brings the controller and the NimBLE host up (the controller may touch the PHY data in NVS, so
// flash access stays off PSRAM stacks), then starts ble_link: a PSRAM stack of 8,192 B (the size the TLS handshake ran on over Wi-Fi), whose call graph
// is NimBLE host calls (they queue to the host task), the TLS wrapper over cable_xport_ble, cable_link_drain_logs, esp_random and ESP_LOG: no nvs_,
// config_, esp_ota, esp_partition or spi_flash call. The key is read once, copied to the TLS wrapper and wiped.
//
// The states, deadlines and the accept policy are ble_state.c (pure, host-tested). This file only turns NimBLE events into those events and acts on them:
//   advertise (50 ms interval, name and UUID of the spec) while ADVERTISING; stop after 30 s with nobody (NOT_FOUND, "Phone not found", no restart by
//   itself); ask for DLE 251 one second after a connect and NEVER for the 2M PHY (finding FB1); drop a link that is not encrypted after 3 s (the deadline waits while
//   the pairing window is open or a number is on show); ask a peer to encrypt only while the window is open or for a bonded peer; open the pairing window by
//   itself when the link starts with no bond stored; accept the L2CAP channel only from an encrypted, bonded peer (ble_accept_check) unless this build has BLE_DEV_ALLOW_UNBONDED; run the TLS handshake when the
//   channel opens (5 s); on three alerts in a row close the link and wait out the refused back-off before advertising again.
//
// BLE_DEV_ALLOW_UNBONDED=1 is a build flag for the walking skeleton (plan question QQ5): TLS-PSK still authenticates both ends, and the link is
// accepted without a bond. It is a CMake variable (-DBLE_DEV_ALLOW_UNBONDED=1 on the idf.py command line), never set in a defaults file or in the
// CMakeLists, and a build that has it logs a warning at every start (test_ble_link_source.py holds all three).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ble_state.h"
#include "cable_xport.h"

#ifdef DEVICE_BLE_LINK

// True when the phone mode is on and has been started (the host synced, the link task runs). False means no BLE was started.
bool ble_link_start(void);
// The transport to give cable_client_start_with() when ble_link_start() returned true.
const cable_xport_t *ble_link_xport(void);
// The link state, for the Link screen and the logs. BLE_ST_OFF when not started. Cheap; any task.
ble_state_t ble_link_state(void);
// Open (true) or close (false) the pairing window: new bonds are made only inside it, for BLE_PAIR_WINDOW_MS (120 s). Closed by default; the link
// opens it by itself when it starts with no bond stored, and the Phone screen's [pair] button opens it. No-op when the phone mode is not running. Any task.
void ble_link_pairing_window(bool open);
// The pairing screen's three calls (B5; the screen itself is a separate task). The number: true while a pairing by numeric comparison is waiting for Yes or
// No, with the 6 digits to show (the window is open, spec section 4). The answer: Yes accepts, No rejects, closes the window and ends the link; a pairing
// that no longer has a window is rejected whatever the answer. Busy: the window is open or a number is on show (the mode switch waits, MS_BUSY_PAIRING).
// Any task, none of them writes flash.
bool ble_link_pair_prompt(uint32_t *number);
void ble_link_pair_answer(bool yes);
bool ble_link_pairing_busy(void);
// "Forget": closes the window, rejects an offer, wipes ALL bonds once (ble_store_clear) and ends the link. It only POSTS the request: the wipe runs on the NimBLE
// host task, which owns the bond store (review M3). It writes no flash on the caller's stack; any task may call it. A second call before the first has run is a
// second wipe, which does nothing more.
void ble_link_forget(void);
// The Phone screen's two reads. Bond count: how many bonds are stored (0 to the store's limit, 2). Left: milliseconds of the pairing window still to run, 0 when it is closed or over. Any task, cheap.
unsigned ble_link_bond_count(void);
uint32_t ble_link_pairing_left_ms(void);
// What the phone wrote to the two characteristics (spec section 5; only a peer that passes the channel rule may write). The relay status is the byte of the
// spec table (ble_relay_status_name); the home hint is true when the phone sees the home network. Any task; cheap.
uint8_t ble_link_relay_status(void);
bool ble_link_home_hint(void);
// Begin advertising again after NOT_FOUND ("Phone not found") or an earlier stop: a START event. False when the phone mode is not running. Any task.
bool ble_link_restart(void);

#else

static inline bool ble_link_start(void) { return false; }
static inline const cable_xport_t *ble_link_xport(void) { return NULL; }
static inline ble_state_t ble_link_state(void) { return BLE_ST_OFF; }
static inline bool ble_link_restart(void) { return false; }
static inline bool ble_link_pair_prompt(uint32_t *number) { (void)number; return false; }
static inline void ble_link_pair_answer(bool yes) { (void)yes; }
static inline bool ble_link_pairing_busy(void) { return false; }
static inline void ble_link_forget(void) {}
static inline unsigned ble_link_bond_count(void) { return 0; }
static inline uint32_t ble_link_pairing_left_ms(void) { return 0; }
static inline uint8_t ble_link_relay_status(void) { return 0; }
static inline bool ble_link_home_hint(void) { return false; }
static inline void ble_link_pairing_window(bool open) { (void)open; }

#endif

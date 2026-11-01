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
//   itself); ask for DLE 251 one second after a connect and NEVER for the 2M PHY (finding FB1); drop a link that is not encrypted after 3 s; accept the
//   L2CAP channel only from an encrypted, bonded peer (ble_accept_check) unless this build has BLE_DEV_ALLOW_UNBONDED; run the TLS handshake when the
//   channel opens (5 s); on three alerts in a row close the link and wait out the refused back-off before advertising again.
//
// BLE_DEV_ALLOW_UNBONDED=1 is a build flag for the walking skeleton (plan question QQ5): TLS-PSK still authenticates both ends, and the link is
// accepted without a bond. It is a CMake variable (-DBLE_DEV_ALLOW_UNBONDED=1 on the idf.py command line), never set in a defaults file or in the
// CMakeLists, and a build that has it logs a warning at every start (test_ble_link_source.py holds all three).
#pragma once

#include <stdbool.h>

#include "ble_state.h"
#include "cable_xport.h"

#ifdef DEVICE_BLE_LINK

// True when the phone mode is on and has been started (the host synced, the link task runs). False means no BLE was started.
bool ble_link_start(void);
// The transport to give cable_client_start_with() when ble_link_start() returned true.
const cable_xport_t *ble_link_xport(void);
// The link state, for the Link screen and the logs. BLE_ST_OFF when not started. Cheap; any task.
ble_state_t ble_link_state(void);
// Begin advertising again after NOT_FOUND ("Phone not found") or an earlier stop: a START event. False when the phone mode is not running. Any task.
bool ble_link_restart(void);

#else

static inline bool ble_link_start(void) { return false; }
static inline const cable_xport_t *ble_link_xport(void) { return NULL; }
static inline ble_state_t ble_link_state(void) { return BLE_ST_OFF; }
static inline bool ble_link_restart(void) { return false; }

#endif

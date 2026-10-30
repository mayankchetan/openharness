// The Wi-Fi station for Wi-Fi mode: esp_netif, the event loop, the driver, one access point. Nothing here runs in USB mode: the only caller of
// nwifi_start() is net_link.c, and only after NVS says wl_mode = 1 (test_net_link_source.py holds both facts).
//
// FLASH SAFETY. Three things in this stack read or write flash, and none of them may run on a PSRAM stack:
//   * nvs_flash_*: not called here. config_store_init() opened NVS at boot, and the credentials were read by config_net_load() before this.
//   * PHY calibration data (the "phy" NVS namespace). CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y, so the first esp_phy_enable() of a boot
//     (inside esp_wifi_start) reads the stored data and, when it is missing or stale, runs a full calibration and WRITES it back
//     (esp_phy_load_cal_and_init: esp_phy_load_cal_data_from_nvs / esp_phy_store_cal_data_to_nvs). Later enables of the same boot do not.
//     That first enable happens inside nwifi_start(), which net_link.c runs on the short-lived internal-stack `net_init` task.
//   * The Wi-Fi driver's own settings store. Off: wifi_init_config_t.nvs_enable = 0 and esp_wifi_set_storage(WIFI_STORAGE_RAM), so it neither
//     reads nor writes NVS and keeps the credentials only in RAM.
// After nwifi_start() returns, everything left (connect, disconnect, the event bits) is flash-free and may be called from the PSRAM-stack
// `net_link` task.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "net_state.h"

// Bits of nwifi_bits().
#define NWIFI_ASSOCIATED   (1u << 0)   // joined the access point
#define NWIFI_GOT_IP       (1u << 1)   // has an IPv4 address
#define NWIFI_DISCONNECTED (1u << 2)   // a disconnect event arrived; cleared by nwifi_connect()

// Called on the event task for every change the state machine cares about (WIFI_DOWN, GOT_IP, LOST_IP). It must be quick and take no lock
// that a task waiting on the event loop could hold.
typedef void (*nwifi_event_cb)(net_event_t event);

// Bring the station up: netif, event loop, hostname dial-<last MAC byte>, driver (6 static RX, 6 TX buffers by sdkconfig), RAM storage,
// WPA2-or-better threshold (open when `pass` is empty), listen interval 3, WIFI_PS_MIN_MODEM, esp_wifi_start(). Does not connect.
// The password is copied into the driver's config and the local copy is zeroed. INTERNAL-STACK TASK ONLY (see FLASH SAFETY above).
bool nwifi_start(const char *ssid, const char *pass, nwifi_event_cb on_event);

// Start joining (esp_wifi_connect). Clears NWIFI_DISCONNECTED first. Safe to repeat while a join is under way.
void nwifi_connect(void);
// Abandon a join or leave the access point.
void nwifi_disconnect(void);

uint32_t nwifi_bits(void);
// Wait until any bit of `mask` is set or `ms` pass; returns all bits as they are on return.
uint32_t nwifi_wait(uint32_t mask, uint32_t ms);
// One log line: signal strength and channel of the access point (no name, no address of the access point).
void nwifi_log_ap(void);

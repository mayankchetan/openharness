// The Wi-Fi station for Wi-Fi mode: esp_netif, the event loop, the driver, one access point. Nothing here runs in USB mode: the only caller of
// nwifi_start() is net_link.c, and only after NVS says wl_mode = 1 (test_net_link_source.py holds both facts).
//
// FLASH SAFETY. Three things in this stack read or write flash, and none of them may run on a PSRAM stack:
//   * nvs_flash_*: not called here. config_store_init() opened NVS at boot, and the credentials were read by config_net_load() before this.
//   * PHY calibration data (the "phy" NVS namespace). CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y, so the first esp_phy_enable() of a boot
//     (inside esp_wifi_start) reads the stored data and, when it is missing or stale, runs a full calibration and WRITES it back
//     (esp_phy_load_cal_and_init: esp_phy_load_cal_data_from_nvs / esp_phy_store_cal_data_to_nvs). Later enables of the same boot do not.
//     That first enable happens inside nwifi_start(), which net_link.c runs on the short-lived internal-stack `net_init` task.
//   * The Wi-Fi driver's own settings store. Off twice: CONFIG_ESP_WIFI_NVS_ENABLED=n (so wifi_init_config_t.nvs_enable is 0 by default) and
//     nvs_enable = 0 plus esp_wifi_set_storage(WIFI_STORAGE_RAM) in code, so it neither reads nor writes NVS and keeps the credentials in RAM only.
// After nwifi_start() returns, everything left (connect, disconnect, the event bits) is flash-free and may be called from the PSRAM-stack
// `net_link` task.
//
// LOG LEVELS. The closed Wi-Fi libraries (libnet80211.a, libpp.a) write through esp_log under their own tags, and those lines reach the cable_link
// log hook and so the daemon's dial log. A scan of the strings in libnet80211.a (IDF 5.5.6, esp32s3) finds format strings that carry the network name,
// the access point's MAC address (BSSID) or, in one debug dump in ieee80211_ioctl.o, the password: "connected with %s, aid = %d, channel %d, %s,
// bssid = ..." (wl_cnx.o), "scan specific ssid=%s", "ssid=%s, channel=%d ...", "history=%d, ssid=%s", "bssid=...", "add bssid ... to blacklist",
// "Invalid AP(<bssid>) SSID=%s: ...", "[idx:%d]SSID:[%d]%s, PWD:%s". The level of each line is not recorded in the string, and the tag is set by the
// closed code (the console shows these as "wifi:" lines). So nwifi_start() sets "wifi", and defensively "net80211" and "pp", to WARN before the
// driver starts. What that does and does not promise: INFO and DEBUG lines of those tags are gone (that holds "connected with <name>... bssid",
// the scan lines and, if it is logged below WARN, the password dump). WARN and ERROR lines still pass, and the "Invalid AP(<bssid>) SSID=<name>"
// family, which the driver prints for an access point with a broken security configuration, may carry the name and the BSSID. This module's own
// lines use the tag "nwifi" and carry lengths, channels, reasons and the dial's own IP address, never the name, the password or the BSSID.
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
// WPA2-or-better threshold (open when `pass` is empty), listen interval 3, log levels (above), WIFI_PS_MIN_MODEM, esp_wifi_start(). Does not connect.
// The password is copied into the driver's config and the local copy is zeroed. Every step is checked; on a failure whatever was made is taken
// down again (driver stopped and deinitialised where it exists, handlers removed, netif freed) and the result is false. INTERNAL-STACK TASK ONLY
// (see FLASH SAFETY above).
bool nwifi_start(const char *ssid, const char *pass, nwifi_event_cb on_event);

// Take the station down after a successful nwifi_start(): esp_wifi_stop, esp_wifi_deinit, handlers off the event loop, netif freed. For the
// fallback to USB mode when Wi-Fi mode cannot go on. Internal-stack task only, and only when no other task is using the radio.
void nwifi_stop(void);

// Start joining (esp_wifi_connect). Clears NWIFI_DISCONNECTED first. Safe to repeat while a join is under way.
void nwifi_connect(void);
// Abandon a join or leave the access point.
void nwifi_disconnect(void);

uint32_t nwifi_bits(void);
// Wait until any bit of `mask` is set or `ms` pass; returns all bits as they are on return.
uint32_t nwifi_wait(uint32_t mask, uint32_t ms);
// One log line: signal strength and channel of the access point (no name, no address of the access point).
void nwifi_log_ap(void);

// HOOK 1 of the battery plan (task W13; docs/specs/cable-wifi-link.md, "Keepalive timing", and the Wi-Fi mode spec section 9). It calls
// esp_wifi_set_ps() and nothing else of the driver: it does NOT touch the listen interval, which is a field of the association request and is
// set once, in nwifi_start(); changing it later would mean disconnecting and re-associating, which drops the TLS session. WIFI_PS_NONE is never
// used. Today both profiles are WIFI_PS_MIN_MODEM (the mode every RTT number was measured with): ACTIVE = wake every DTIM; IDLE (screen asleep)
// is the battery plan's to set, and nothing calls it with IDLE yet. Needs a started driver; false on a bad profile or a refused call (logged).
typedef enum { NET_POWER_ACTIVE, NET_POWER_IDLE } net_power_t;
bool net_power_profile(net_power_t profile);

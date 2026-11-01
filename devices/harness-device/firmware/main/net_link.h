// Wi-Fi mode: the join, the TCP connection, the TLS handshake, and the reconnect loop that keeps them up. (cable-wifi-link.md section 4.3, task W11.)
//
//   app_main.c:   const bool wifi = net_link_start();
//                 cable_client_start_with(wifi ? net_link_xport() : cable_xport_usb());
//
// net_link_start() is the one switch between the two modes. It reads NVS (config_net_load); unless wl_mode is 1 and the configuration is whole it
// returns false having done nothing else: no radio call, no netif, no lwIP, no mbedTLS object, no task. USB mode is exactly what it was.
//
// Two tasks, with different stacks for a reason (also pinned by test_psram_task_sites.py and test_net_link_source.py):
//   net_init   INTERNAL stack, 5,120 B, lives a few hundred milliseconds. Runs nwifi_start(): esp_wifi_init, and esp_wifi_start, whose first
//              PHY enable of the boot may read and write the "phy" NVS namespace (calibration data). Flash operations stay off PSRAM stacks.
//   net_link   PSRAM stack, 8,192 B (the size the bench ran TLS on), runs for ever. Its call graph is: esp_wifi_connect / disconnect (messages to
//              the Wi-Fi task), lwIP sockets, the TLS handshake and record layer (mbedTLS with the ESP hardware crypto, which handles buffers in
//              external RAM), cable_link_drain_logs, esp_random, ESP_LOG, event-group waits. No nvs_, config_, esp_ota, esp_partition, esp_flash or
//              spi_flash call is reachable from it, and it never exits. Credentials were copied to the wrapper and the driver by net_init and
//              scrubbed; net_link reads nothing from NVS.
//
// The transport cable_link sees is a thin "wifi" transport (net_link_xport): open() is a no-op because net_link owns the connection, and read,
// write and is_up pass through to the TLS wrapper over cable_xport_tcp. Reads on a link that is down wait out their time and return -1.
#pragma once

#include <stdbool.h>

#include "cable_xport.h"
#include "net_state.h"

// True when Wi-Fi mode is on and has been started (net_init finished, net_link running). False means USB mode: nothing was started.
bool net_link_start(void);

// The transport to give cable_client_start_with() when net_link_start() returned true.
const cable_xport_t *net_link_xport(void);

// The battery power-off (power_off.c, DEVICE_BATTERY_POWEROFF) closes the link on purpose before the PMIC cuts the power, so the daemon logs a clean
// close and not a drop (and does not wait out its silence window). net_link_close_for_power_off() says goodbye to the daemon when a session is up
// (cable_client_say_bye), closes the TLS connection (close_notify, best effort, 100 ms), and parks the net_link task so it does not reconnect.
// Returns true when Wi-Fi mode is running (there was a link to close, or one that was being made); false in USB mode, where it does nothing. Call it
// from an internal-stack task, once the decision to power off is final. The goodbye and the close usually take a few
// milliseconds. NOT bounded by a second: tls_close takes the transport's lock, which a TCP connect or a handshake in progress holds for up to 5 s
// (CONNECT_MS in cable_xport_tcp.c) and a stalled write for up to 2 s; power_off.c therefore looks at VBUS again after the close (S8 review M1). net_link_resume() un-parks the task (the PMIC did not take the power-off).
bool net_link_close_for_power_off(void);
void net_link_resume(void);

// What the glass says under the wordmark while the dial is not connected: a short line for the state (joining, no address, daemon not
// reachable, key refused), or NULL (USB mode, or the link is up). Cheap; any task.
const char *net_link_status_text(void);

// The link state itself (net_state.h), for the Link screen. NET_OFF in USB mode. Cheap; any task.
net_state_t net_link_state(void);

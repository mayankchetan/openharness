// The dial's own settings, in NVS.
//
// Local display, sound, gesture, voice-language and screen-lock preferences. Everything this file
// used to hold — the WiFi networks, the backend URL, the device
// token, the SDS provisioning blob, the E2EE identity and its pinned peers, the last-selected machine —
// existed so the device could find a network and prove who it was to a backend. A cable answers both, and
// the account lives on the computer at the other end of it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_VLANG_MAX 8    // "en" / "vi" + nul, with room

// Open NVS. Call once, early — everything below is a no-op until it has run.
void config_store_init(void);

// Screen brightness, 0..255. The renderer applies it to the OLED panel.
uint8_t config_load_brightness(void);
void    config_save_brightness(uint8_t level);

// Habitat starts muted. The saved preference survives firmware updates.
bool config_load_muted(void);
bool config_save_muted(bool muted);

// Local Habitat experiments: bit 0 = legacy focus face, bit 2 = quiet reactions,
// bit 3 = straight title (default: curved), bit 4 = do not follow the desktop companion.
//
// BIT 1 IS RETIRED, NOT FREE. It was rim scrolling — drag a finger around the bezel — and devices in
// the field still hold it set. A new preference given that bit would inherit their answer, so the
// next one takes bit 4.
uint8_t config_load_habitat_options(void);
bool config_save_habitat_options(uint8_t options);

// Stable character id. Missing values use the image's default; the UI validates ids.
uint8_t config_load_habitat_character(uint8_t fallback);
bool config_save_habitat_character(uint8_t character);

// The voice language the dial captures in. The daemon PROPOSES one from the computer's locale on every
// `welcome`; once the user has picked here, this wins — the person holding the dial may well speak
// something other than the laptop is set to.
void config_load_voicelang(char *out, size_t cap);
void config_save_voicelang(const char *lang);

// Which way a drag on the dial moves the window's scrollback. False (the default, and what every build
// before this one did): the text follows the finger. True: the view does.
bool config_load_scroll_reversed(void);
void config_save_scroll_reversed(bool reversed);

// Which way a horizontal swipe walks the carousel. False (the default): a swipe left goes to the NEXT
// agent in the list. True: it goes to the previous one.
bool config_load_swipe_reversed(void);
void config_save_swipe_reversed(bool reversed);

// ── Wi-Fi settings (docs/specs/cable-wifi-link.md section 3) ──────────────────────────────────────────
// The network to join, the address of the daemon's listener and the key (TLS-PSK) the dial proves to it, in NVS namespace "pair":
// wl_ssid, wl_pass, wl_host, wl_port, wl_key and wl_mode, and wl_ver = 1, the COMMIT MARKER. They are given over the USB cable
// only (cable_client.c refuses them on any other link) and never logged: a log line may carry a length, never a value.
//
// The marker rules, which the tests hold to (test_net_config.c):
//   - a set removes wl_ver FIRST, writes the other keys, and writes wl_ver LAST. A power cut between two keys leaves wl_ver
//     absent, so the dial is unprovisioned and never holds half of one network and half of another;
//   - a clear removes wl_ver FIRST for the same reason;
//   - config_net_load / config_net_info say "provisioned" only when wl_ver is 1 AND every key is present and valid;
//   - config_clear_all (factory reset) takes every key of the namespace, these included.
#define CFG_NET_SSID_MAX 32
#define CFG_NET_PASS_MAX 63
#define CFG_NET_HOST_MAX 15
#define CFG_NET_KEY_LEN  32
#define CFG_NET_PORT_MIN 1024

// Why a request was refused. The names (config_net_err_name) are what net.state sends back as `err`: field names or reasons, never values.
typedef enum {
    CFG_NET_OK = 0,
    CFG_NET_E_SSID,
    CFG_NET_E_PASS,
    CFG_NET_E_HOST,
    CFG_NET_E_PORT,
    CFG_NET_E_KEY,
    CFG_NET_E_MODE,
    CFG_NET_E_NOT_PROVISIONED,   // net.key / net.host / net.mode on a dial that holds no whole configuration
    CFG_NET_E_STORAGE,           // NVS would not take a write
} config_net_err_t;

const char *config_net_err_name(config_net_err_t err);

// Everything, secrets included: for the net task that joins the network (W11), nothing else. False unless the dial is provisioned.
typedef struct {
    char    ssid[CFG_NET_SSID_MAX + 1];
    char    pass[CFG_NET_PASS_MAX + 1];
    char    host[CFG_NET_HOST_MAX + 1];
    uint16_t port;
    uint8_t key[CFG_NET_KEY_LEN];
    uint8_t mode;                // wl_mode: 1 = join Wi-Fi when the USB cable is unplugged, 0 = USB only
} config_net_t;
bool config_net_load(config_net_t *out);

// What may be sent to the daemon and shown: no password and no key.
typedef struct {
    bool     saved;
    char     ssid[CFG_NET_SSID_MAX + 1];
    char     host[CFG_NET_HOST_MAX + 1];
    uint16_t port;
    uint8_t  mode;
} config_net_info_t;
void config_net_info(config_net_info_t *out);

// The standard base64 of exactly 32 bytes (44 characters, one "=", no other padding, no whitespace, no url-safe letters, trailing bits zero).
bool config_net_key_decode(const char *b64, uint8_t out[CFG_NET_KEY_LEN]);

// The checks alone, with no NVS access: the cable reader (a PSRAM stack, which must not write flash) validates a request with these before
// it hands the write to the net_store task. config_net_set and the setters below run the same checks again.
config_net_err_t config_net_check(const char *ssid, const char *pass, const char *host, long port, const uint8_t *key, long mode);
config_net_err_t config_net_check_host(const char *host, long port);
config_net_err_t config_net_check_mode(long mode);

// Check every field, then write all of them, wl_ver last. `port` and `mode` are -1 when the message had none. A refusal names the
// FIRST bad field in the order ssid, pass, host, port, key, mode and writes nothing.
config_net_err_t config_net_set(const char *ssid, const char *pass, const char *host, long port, const uint8_t *key, long mode);
// These three need a whole configuration (CFG_NET_E_NOT_PROVISIONED otherwise).
config_net_err_t config_net_set_key(const uint8_t *key);
config_net_err_t config_net_set_host(const char *host, long port);
config_net_err_t config_net_set_mode(long mode);
// Forget the network, the address, the key and the mode. Fine when there is nothing to forget.
config_net_err_t config_net_clear(void);

// Factory reset (BOOT held at power-on, or Settings → Reset): forget all of the above, and the Wi-Fi settings.
bool config_clear_all(void);

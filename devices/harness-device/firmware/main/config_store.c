#include "config_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ram_telemetry.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "store_gate.h"

static const char *TAG = "config";
static const char *NS = "pair";

static void read_str(nvs_handle_t h, const char *key, char *dst, size_t cap);   // defined below

// Every write opens through the store gate: when the dial is about to be switched off by the PMIC (power_off.c) the gate stops new writers
// and waits for the one in flight, so the power goes after the commit and never in the middle of a flash write. A refused open is a write
// that did not happen (the callers already handle a failed open).
static esp_err_t open_rw(nvs_handle_t *h)
{
    if (!store_gate_enter()) return ESP_ERR_INVALID_STATE;
    const esp_err_t e = nvs_open(NS, NVS_READWRITE, h);
    if (e != ESP_OK) store_gate_leave();
    return e;
}
static void close_rw(nvs_handle_t h)
{
    nvs_close(h);
    store_gate_leave();
}


void config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void read_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    memset(dst, 0, cap);
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) dst[0] = '\0';
}

void config_load_voicelang(char *out, size_t cap)
{
    nvs_handle_t h;
    memset(out, 0, cap);
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        read_str(h, "vlang", out, cap);
        nvs_close(h);
    }
    if (out[0] == '\0' && cap > 2) { strncpy(out, "en", cap - 1); out[cap - 1] = '\0'; }   // factory default: English (Settings › Voice flips it)
}

void config_save_voicelang(const char *lang)
{
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return;
    bool ok = nvs_set_str(h, "vlang", lang) == ESP_OK && nvs_commit(h) == ESP_OK;
    close_rw(h);
    ESP_LOGI(TAG, "save_voicelang '%s': %s", lang, ok ? "ok" : "FAILED");
}

uint8_t config_load_brightness(void)
{
    nvs_handle_t h;
    uint8_t v = 204;    // default 80%: a dial nobody has dimmed (owner, 2026-10-01; was 100%)
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "bright", &v);   // leaves v at default if key is absent
        nvs_close(h);
    }
    return v;
}

void config_save_brightness(uint8_t level)
{
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return;
    bool ok = nvs_set_u8(h, "bright", level) == ESP_OK && nvs_commit(h) == ESP_OK;
    close_rw(h);
    ESP_LOGI(TAG, "save_brightness 0x%02x: %s", level, ok ? "ok" : "FAILED");
}

bool config_load_muted(void)
{
    nvs_handle_t h;
    // The prototype starts quiet; a stock build keeps its previous default.
    uint8_t muted = 1;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "muted", &muted);
        nvs_close(h);
    }
    return muted != 0;
}

bool config_save_muted(bool muted)
{
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return false;
    bool ok = nvs_set_u8(h, "muted", muted ? 1 : 0) == ESP_OK && nvs_commit(h) == ESP_OK;
    close_rw(h);
    ESP_LOGI(TAG, "save_muted %d: %s", (int)muted, ok ? "ok" : "FAILED");
    return ok;
}

uint8_t config_load_habitat_options(void)
{
    nvs_handle_t h;
    uint8_t options = 0; // Curved title; straight scrolling; reactions enabled
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "habitat", &options);
        nvs_close(h);
    }
    return options & 15;
}
bool config_save_habitat_options(uint8_t options)
{
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return false;
    bool ok = nvs_set_u8(h, "habitat", options & 15) == ESP_OK && nvs_commit(h) == ESP_OK;
    close_rw(h);
    return ok;
}

uint8_t config_load_habitat_character(uint8_t fallback)
{
    nvs_handle_t h;
    uint8_t value = fallback;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "habitat_char", &value);
        nvs_close(h);
    }
    return value;
}
bool config_save_habitat_character(uint8_t character)
{
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return false;
    bool ok = nvs_set_u8(h, "habitat_char", character) == ESP_OK && nvs_commit(h) == ESP_OK;
    close_rw(h);
    return ok;
}

// --- scroll direction ---------------------------------------------------------------------------
// Which way a drag moves the window's scrollback. A habit, not a fact about the hardware: some hands
// expect the text to follow the finger, others expect the VIEW to follow it, and neither is wrong.
//
// Stored on the DIAL, not in the window, because it belongs to the hand holding this device. Nothing else
// needs to know: the report is signed before it leaves here, so the daemon, the window and the terminal
// all see the same frame they always did. A preference two sides both hold is a preference they can
// disagree about.
bool config_load_scroll_reversed(void)
{
    nvs_handle_t h;
    uint8_t v = 0;   // default: unchanged from every build before this one
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "scrollrev", &v);
        nvs_close(h);
    }
    return v != 0;
}

void config_save_scroll_reversed(bool reversed)
{
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return;
    bool ok = nvs_set_u8(h, "scrollrev", reversed ? 1 : 0) == ESP_OK && nvs_commit(h) == ESP_OK;
    close_rw(h);
    ESP_LOGI(TAG, "save_scroll_reversed %d: %s", (int)reversed, ok ? "ok" : "FAILED");
}

// Which way a horizontal swipe walks the carousel. Same shape as the scroll habit above and stored
// separately on purpose: someone can want the scrollback reversed and the carousel left alone, and one
// switch for both would force a preference nobody asked for.
bool config_load_swipe_reversed(void)
{
    nvs_handle_t h;
    uint8_t v = 0;   // default: unchanged from every build before this one
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "swiperev", &v);
        nvs_close(h);
    }
    return v != 0;
}

void config_save_swipe_reversed(bool reversed)
{
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return;
    bool ok = nvs_set_u8(h, "swiperev", reversed ? 1 : 0) == ESP_OK && nvs_commit(h) == ESP_OK;
    close_rw(h);
    ESP_LOGI(TAG, "save_swipe_reversed %d: %s", (int)reversed, ok ? "ok" : "FAILED");
}

// --- SDS provisioning ---------------------------------------------------------------------------
// One NVS key per value. Do NOT be tempted to fold these into device_net_config_t: that struct is stored
// as a fixed-size blob under "netv", so adding a field silently invalidates every saved network on an
// upgraded unit.
#define K_CID    "cid"
#define K_MQHOST "mqhost"
#define K_MQPORT "mqport"
#define K_MQUSER "mquser"
#define K_MQPASS "mqpass"
#define K_FACH   "fach"
#define K_FDCH   "fdch"

// --- Wi-Fi settings -----------------------------------------------------------------------------
// docs/specs/cable-wifi-link.md section 3, and the marker rules in config_store.h. Nothing below logs a value: a line carries a
// length or a field NAME. Every ESP_LOG line of this firmware is framed to the daemon's dial log (cable_link.c), so a password
// logged here would be in a file.
#define K_WL_SSID "wl_ssid"
#define K_WL_PASS "wl_pass"
#define K_WL_HOST "wl_host"
#define K_WL_PORT "wl_port"
#define K_WL_KEY  "wl_key"
#define K_WL_MODE "wl_mode"
#define K_WL_VER  "wl_ver"
#define WL_VERSION 1
#define K_BL_VER  "bl_ver"    // the commit marker of the phone settings (config_store.h, "Phone mode settings")
#define BL_VERSION 1
#define K_STAY_WL "stay_wl"   // the mode switch's override; net.clear takes it with the network (its accessors are at the end of the file)

const char *config_net_err_name(config_net_err_t err)
{
    switch (err) {
    case CFG_NET_OK:               return "ok";
    case CFG_NET_E_SSID:           return "ssid";
    case CFG_NET_E_PASS:           return "pass";
    case CFG_NET_E_HOST:           return "host";
    case CFG_NET_E_PORT:           return "port";
    case CFG_NET_E_KEY:            return "key";
    case CFG_NET_E_MODE:           return "mode";
    case CFG_NET_E_NOT_PROVISIONED: return "not_provisioned";
    case CFG_NET_E_STORAGE:        return "storage";
    }
    return "storage";
}

static void scrub(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/** The length of `s` up to `cap`: a longer string reads as cap + 1, so a bad one is never walked to its end. */
static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0;
    while (s && n <= cap && s[n]) n++;
    return n;
}

/** Between `min` and `max` bytes with no control byte (below 0x20, or DEL). Any other byte is allowed: a name may be UTF-8. */
static bool text_ok(const char *s, size_t min, size_t max)
{
    if (!s) return false;
    const size_t n = bounded_len(s, max);
    if (n < min || n > max) return false;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) return false;
    return true;
}

/** Four decimal parts of 1 to 3 digits, none above 255, no leading zero but a lone "0", nothing else. */
static bool ipv4_ok(const char *s)
{
    if (!s) return false;
    int parts = 0;
    const char *p = s;
    for (;;) {
        if (*p < '0' || *p > '9') return false;
        const char first = *p;
        int digits = 0, value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            if (++digits > 3) return false;
            p++;
        }
        if ((digits > 1 && first == '0') || value > 255) return false;
        parts++;
        if (*p == '.') { if (parts == 4) return false; p++; continue; }
        return *p == 0 && parts == 4;
    }
}

/** An address the dial could never be sent to the daemon at: 0.0.0.0, 255.255.255.255, loopback (127/8) and multicast (224/4). */
static bool host_unusable(const char *s)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return true;
    return (a == 0 && b == 0 && c == 0 && d == 0) || (a == 255 && b == 255 && c == 255 && d == 255) || a == 127 || (a >= 224 && a <= 239);
}

static config_net_err_t host_port_check(const char *host, long port)
{
    if (!ipv4_ok(host) || host_unusable(host)) return CFG_NET_E_HOST;
    if (port < CFG_NET_PORT_MIN || port > 65535) return CFG_NET_E_PORT;
    return CFG_NET_OK;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool b64_decode32(const char *b64, uint8_t *bytes)
{
    if (!b64 || bounded_len(b64, 44) != 44 || b64[43] != '=') return false;
    size_t o = 0;
    for (size_t i = 0; i < 40; i += 4) {   // ten whole groups: 30 bytes
        const int a = b64_value(b64[i]), b = b64_value(b64[i + 1]), c = b64_value(b64[i + 2]), d = b64_value(b64[i + 3]);
        if ((a | b | c | d) < 0) return false;
        bytes[o++] = (uint8_t)((a << 2) | (b >> 4));
        bytes[o++] = (uint8_t)(((b & 15) << 4) | (c >> 2));
        bytes[o++] = (uint8_t)(((c & 3) << 6) | d);
    }
    const int a = b64_value(b64[40]), b = b64_value(b64[41]), c = b64_value(b64[42]);
    // the last group is two bytes and one "=", and its unused bits must be zero: one spelling per key
    if ((a | b | c) < 0 || (c & 3)) return false;
    bytes[o++] = (uint8_t)((a << 2) | (b >> 4));
    bytes[o++] = (uint8_t)(((b & 15) << 4) | (c >> 2));
    return true;
}

bool config_net_key_decode(const char *b64, uint8_t out[CFG_NET_KEY_LEN])
{
    // The scratch holds half a key when a decode fails part-way: it is overwritten on every path.
    uint8_t bytes[CFG_NET_KEY_LEN];
    const bool ok = b64_decode32(b64, bytes);
    if (ok) memcpy(out, bytes, CFG_NET_KEY_LEN);
    scrub(bytes, sizeof bytes);
    return ok;
}

static bool wl_get_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    memset(dst, 0, cap);
    return nvs_get_str(h, key, dst, &len) == ESP_OK;
}

bool config_net_load(config_net_t *out)
{
    memset(out, 0, sizeof *out);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t version = 0, mode = 0;
    size_t key_len = sizeof out->key;
    // The marker first, then every key: a marker with a key missing is not a configuration.
    bool ok = nvs_get_u8(h, K_WL_VER, &version) == ESP_OK && version == WL_VERSION
        && wl_get_str(h, K_WL_SSID, out->ssid, sizeof out->ssid)
        && wl_get_str(h, K_WL_PASS, out->pass, sizeof out->pass)
        && wl_get_str(h, K_WL_HOST, out->host, sizeof out->host)
        && nvs_get_u16(h, K_WL_PORT, &out->port) == ESP_OK
        && nvs_get_blob(h, K_WL_KEY, out->key, &key_len) == ESP_OK && key_len == sizeof out->key;
    if (ok) nvs_get_u8(h, K_WL_MODE, &mode);   // absent reads as USB only
    nvs_close(h);
    ok = ok && text_ok(out->ssid, 1, CFG_NET_SSID_MAX)
        && (out->pass[0] == 0 || text_ok(out->pass, 8, CFG_NET_PASS_MAX))
        && host_port_check(out->host, out->port) == CFG_NET_OK;
    out->mode = mode == 1 ? 1 : 0;
    if (!ok) scrub(out, sizeof *out);
    return ok;
}

void config_net_info(config_net_info_t *out)
{
    memset(out, 0, sizeof *out);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t version = 0, mode = 0;
    uint16_t port = 0;
    size_t pass_len = 0, key_len = 0;
    // The password and the key are never read here: their presence and size are asked of NVS (a NULL buffer), which is all this needs
    // to say "provisioned". A password is 0 or 8 to 63 bytes, held with its terminator.
    bool ok = nvs_get_u8(h, K_WL_VER, &version) == ESP_OK && version == WL_VERSION
        && wl_get_str(h, K_WL_SSID, out->ssid, sizeof out->ssid)
        && wl_get_str(h, K_WL_HOST, out->host, sizeof out->host)
        && nvs_get_u16(h, K_WL_PORT, &port) == ESP_OK
        && nvs_get_str(h, K_WL_PASS, NULL, &pass_len) == ESP_OK && (pass_len == 1 || (pass_len >= 9 && pass_len <= CFG_NET_PASS_MAX + 1))
        && nvs_get_blob(h, K_WL_KEY, NULL, &key_len) == ESP_OK && key_len == CFG_NET_KEY_LEN;
    // The phone settings: the marker and the SIZE of the key (the key itself is not read here either).
    uint8_t bl_version = 0;
    size_t bl_key_len = 0;
    const bool ble = nvs_get_u8(h, K_BL_VER, &bl_version) == ESP_OK && bl_version == BL_VERSION
        && nvs_get_blob(h, K_WL_KEY, NULL, &bl_key_len) == ESP_OK && bl_key_len == CFG_NET_KEY_LEN;
    if (ok || ble) nvs_get_u8(h, K_WL_MODE, &mode);   // absent reads as USB only
    nvs_close(h);
    const bool wifi = ok && text_ok(out->ssid, 1, CFG_NET_SSID_MAX) && host_port_check(out->host, port) == CFG_NET_OK;
    if (!wifi) memset(out, 0, sizeof *out);
    out->ble = ble;
    out->saved = wifi;
    if (wifi) out->port = port;
    // Each mode counts only with its own whole settings: the Wi-Fi mode over phone settings alone, or the phone mode over Wi-Fi settings alone, is 0.
    out->mode = (mode == 1 && wifi) ? 1 : (mode == 2 && ble) ? 2 : 0;
}

/** A key that is not there is not an error to erase. */
static bool erased(esp_err_t err) { return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND; }

config_net_err_t config_net_check(const char *ssid, const char *pass, const char *host, long port, const uint8_t *key, long mode)
{
    if (!text_ok(ssid, 1, CFG_NET_SSID_MAX)) return CFG_NET_E_SSID;
    if (!pass || (pass[0] != 0 && !text_ok(pass, 8, CFG_NET_PASS_MAX))) return CFG_NET_E_PASS;
    const config_net_err_t where = host_port_check(host, port);
    if (where != CFG_NET_OK) return where;
    if (config_net_check_key(key) != CFG_NET_OK) return CFG_NET_E_KEY;
    if (mode != 0 && mode != 1) return CFG_NET_E_MODE;
    return CFG_NET_OK;
}

/** A key must be there and must not be all zero: that is what an uninitialised buffer looks like, not a key anyone drew. */
config_net_err_t config_net_check_key(const uint8_t *key)
{
    if (!key) return CFG_NET_E_KEY;
    for (size_t i = 0; i < CFG_NET_KEY_LEN; i++) if (key[i]) return CFG_NET_OK;
    return CFG_NET_E_KEY;
}

/** First step of every change to a held configuration: the marker goes, and the removal is committed before anything else is written. */
static bool marker_off(nvs_handle_t h)
{
    return erased(nvs_erase_key(h, K_WL_VER)) && nvs_commit(h) == ESP_OK;
}

/**
 * Last step: the marker comes back, and is committed. If that commit fails the settings are whole, yet the reply will say they are not;
 * the marker is taken back if the store allows, so that "refused" and "unprovisioned" mean the same thing.
 */
static bool marker_on(nvs_handle_t h)
{
    if (nvs_set_u8(h, K_WL_VER, WL_VERSION) != ESP_OK) return false;
    if (nvs_commit(h) == ESP_OK) return true;
    if (erased(nvs_erase_key(h, K_WL_VER))) nvs_commit(h);
    return false;
}

/** marker_off / marker_on for either or both configurations: wl_ver (Wi-Fi) and bl_ver (phone). With only the Wi-Fi marker they do what marker_off and marker_on do. */
static bool markers_off(nvs_handle_t h, bool wl, bool bl)
{
    return (!wl || erased(nvs_erase_key(h, K_WL_VER))) && (!bl || erased(nvs_erase_key(h, K_BL_VER))) && nvs_commit(h) == ESP_OK;
}

static bool markers_on(nvs_handle_t h, bool wl, bool bl)
{
    const bool set = (!wl || nvs_set_u8(h, K_WL_VER, WL_VERSION) == ESP_OK) && (!bl || nvs_set_u8(h, K_BL_VER, BL_VERSION) == ESP_OK);
    if (set && nvs_commit(h) == ESP_OK) return true;
    // Take the markers back if the store allows, so that "refused" and "unprovisioned" mean the same thing.
    if (erased(nvs_erase_key(h, K_WL_VER)) && erased(nvs_erase_key(h, K_BL_VER))) nvs_commit(h);
    return false;
}

config_net_err_t config_net_check_host(const char *host, long port) { return host_port_check(host, port); }

config_net_err_t config_net_check_mode(long mode) { return (mode == 0 || mode == 1 || mode == 2) ? CFG_NET_OK : CFG_NET_E_MODE; }

config_net_err_t config_net_set(const char *ssid, const char *pass, const char *host, long port, const uint8_t *key, long mode)
{
    const config_net_err_t err = config_net_check(ssid, pass, host, port, key, mode);
    ESP_LOGI(TAG, "net set: ssid_len=%u pass_len=%u host_len=%u -> %s",
             (unsigned)bounded_len(ssid, 64), (unsigned)bounded_len(pass, 128), (unsigned)bounded_len(host, 32), config_net_err_name(err));
    if (err != CFG_NET_OK) return err;
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return CFG_NET_E_STORAGE;
    // The marker goes first, so a cut anywhere below leaves the dial unprovisioned, and comes back last.
    const bool ok = marker_off(h)
        && nvs_set_str(h, K_WL_SSID, ssid) == ESP_OK
        && nvs_set_str(h, K_WL_PASS, pass) == ESP_OK
        && nvs_set_str(h, K_WL_HOST, host) == ESP_OK
        && nvs_set_u16(h, K_WL_PORT, (uint16_t)port) == ESP_OK
        && nvs_set_blob(h, K_WL_KEY, key, CFG_NET_KEY_LEN) == ESP_OK
        && nvs_set_u8(h, K_WL_MODE, (uint8_t)mode) == ESP_OK
        && nvs_commit(h) == ESP_OK
        && marker_on(h);
    close_rw(h);
    return ok ? CFG_NET_OK : CFG_NET_E_STORAGE;
}

static bool provisioned(void)
{
    config_net_info_t info;
    config_net_info(&info);
    return info.saved;
}

config_net_err_t config_net_set_key(const uint8_t *key)
{
    if (config_net_check_key(key) != CFG_NET_OK) return CFG_NET_E_KEY;
    if (!provisioned()) return CFG_NET_E_NOT_PROVISIONED;
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return CFG_NET_E_STORAGE;
    // Under the marker rule like a set: a cut between two writes of a change leaves the dial unprovisioned, never half changed.
    const bool ok = marker_off(h) && nvs_set_blob(h, K_WL_KEY, key, CFG_NET_KEY_LEN) == ESP_OK && nvs_commit(h) == ESP_OK && marker_on(h);
    close_rw(h);
    ESP_LOGI(TAG, "net key: %s", ok ? "replaced" : "FAILED");
    return ok ? CFG_NET_OK : CFG_NET_E_STORAGE;
}

config_net_err_t config_net_set_host(const char *host, long port)
{
    const config_net_err_t err = host_port_check(host, port);
    if (err != CFG_NET_OK) return err;
    if (!provisioned()) return CFG_NET_E_NOT_PROVISIONED;
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return CFG_NET_E_STORAGE;
    const bool ok = marker_off(h) && nvs_set_str(h, K_WL_HOST, host) == ESP_OK && nvs_set_u16(h, K_WL_PORT, (uint16_t)port) == ESP_OK
        && nvs_commit(h) == ESP_OK && marker_on(h);
    close_rw(h);
    ESP_LOGI(TAG, "net host: host_len=%u %s", (unsigned)bounded_len(host, 32), ok ? "replaced" : "FAILED");
    return ok ? CFG_NET_OK : CFG_NET_E_STORAGE;
}

config_net_err_t config_net_set_mode(long mode)
{
    if (config_net_check_mode(mode) != CFG_NET_OK) return CFG_NET_E_MODE;
    config_net_info_t info;
    config_net_info(&info);
    // Each mode needs its own settings: 1 the Wi-Fi settings, 2 the phone settings, 0 (USB only) either. Mode 2 is never stored without bl_ver and a key.
    const bool allowed = mode == 1 ? info.saved : mode == 2 ? info.ble : (info.saved || info.ble);
    if (!allowed) return CFG_NET_E_NOT_PROVISIONED;
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return CFG_NET_E_STORAGE;
    // Every marker that is on comes off for the write and back at the end, so a cut leaves no half-changed configuration.
    const bool ok = markers_off(h, info.saved, info.ble) && nvs_set_u8(h, K_WL_MODE, (uint8_t)mode) == ESP_OK && nvs_commit(h) == ESP_OK
        && markers_on(h, info.saved, info.ble);
    close_rw(h);
    ESP_LOGI(TAG, "net mode %d: %s", (int)mode, ok ? "ok" : "FAILED");
    return ok ? CFG_NET_OK : CFG_NET_E_STORAGE;
}

bool config_ble_load(config_ble_t *out)
{
    memset(out, 0, sizeof *out);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t version = 0;
    size_t key_len = sizeof out->key;
    bool ok = nvs_get_u8(h, K_BL_VER, &version) == ESP_OK && version == BL_VERSION
        && nvs_get_blob(h, K_WL_KEY, out->key, &key_len) == ESP_OK && key_len == sizeof out->key;
    nvs_close(h);
    ok = ok && config_net_check_key(out->key) == CFG_NET_OK;
    if (!ok) scrub(out, sizeof *out);
    return ok;
}

config_net_err_t config_net_set_ble(const uint8_t *key)
{
    const config_net_err_t err = config_net_check_key(key);
    ESP_LOGI(TAG, "net set ble: -> %s", config_net_err_name(err));
    if (err != CFG_NET_OK) return err;
    // The Wi-Fi settings share this key: if they are whole, their marker comes off for the write too and back at the end.
    config_net_info_t info;
    config_net_info(&info);
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return CFG_NET_E_STORAGE;
    const bool ok = markers_off(h, info.saved, true)
        && nvs_set_blob(h, K_WL_KEY, key, CFG_NET_KEY_LEN) == ESP_OK
        && nvs_commit(h) == ESP_OK
        && markers_on(h, info.saved, true);
    close_rw(h);
    return ok ? CFG_NET_OK : CFG_NET_E_STORAGE;
}

// The NimBLE host keeps its bonds in NVS namespace "nimble_bond" (ble_store_nvs.c, CONFIG_BT_NIMBLE_NVS_PERSIST). That store is not ours, but it is part of what
// "forget this dial's network" and the factory reset must forget: a dial handed on would otherwise still trust the old owner's phone. Erasing the namespace does
// not need the host running (it reads its RAM copy from here at the next boot; a dial that is switched to USB by a clear or a reset reboots anyway).
// Through the store gate, like every write. A namespace that does not exist holds nothing to erase.
#define NIMBLE_BOND_NS "nimble_bond"
static bool ble_bonds_erase(void)
{
    nvs_handle_t h;
    const esp_err_t probe = nvs_open(NIMBLE_BOND_NS, NVS_READONLY, &h);
    if (probe == ESP_ERR_NVS_NOT_FOUND) return true;
    if (probe == ESP_OK) nvs_close(h);
    if (!store_gate_enter()) return false;
    bool ok = nvs_open(NIMBLE_BOND_NS, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    store_gate_leave();
    ESP_LOGW(TAG, "bond store erase: %s", ok ? "ok" : "FAILED");
    return ok;
}

config_net_err_t config_net_clear(void)
{
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return CFG_NET_E_STORAGE;
    // The marker first, for the same reason as in config_net_set.
    bool ok = erased(nvs_erase_key(h, K_WL_VER)) && erased(nvs_erase_key(h, K_BL_VER)) && nvs_commit(h) == ESP_OK   // both markers, before any key
        && erased(nvs_erase_key(h, K_WL_SSID)) && erased(nvs_erase_key(h, K_WL_PASS)) && erased(nvs_erase_key(h, K_WL_HOST))
        && erased(nvs_erase_key(h, K_WL_PORT)) && erased(nvs_erase_key(h, K_WL_KEY)) && erased(nvs_erase_key(h, K_WL_MODE))
        && erased(nvs_erase_key(h, K_STAY_WL))   // the override means nothing without a network, and must not outlive it
        && nvs_commit(h) == ESP_OK;
    close_rw(h);
    ok = ok && ble_bonds_erase();   // last: the settings and their markers are gone first, so a cut here leaves no target (and says so)
    ESP_LOGW(TAG, "net clear: %s", ok ? "ok" : "FAILED");
    return ok ? CFG_NET_OK : CFG_NET_E_STORAGE;
}

bool config_clear_all(void)
{
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return false;
    nvs_erase_all(h);
    bool ok = nvs_commit(h) == ESP_OK;
    close_rw(h);
    ok = ble_bonds_erase() && ok;   // the phones' bonds too (run both even if the first failed)
    ESP_LOGW(TAG, "clear_all: %s", ok ? "ok" : "FAILED");
    return ok;
}

// ── the mode switch's values (config_store.h) ──────────────────────────────────────────────────────
#define K_BOOT_USB "boot_usb"

/** True only for the byte 1. Absent, corrupt, another type or an unreadable store: false. */
static bool flag_load(const char *flag)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    const bool ok = nvs_get_u8(h, flag, &v) == ESP_OK;
    nvs_close(h);
    return ok && v == 1;
}

static bool flag_save(const char *flag, bool on)
{
    // Unchanged: no write. (A corrupt byte reads false, so asking for false leaves it where it is; it is harmless and reads as the default.)
    if (flag_load(flag) == on) return true;
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) return false;
    const bool ok = nvs_set_u8(h, flag, on ? 1 : 0) == ESP_OK && nvs_commit(h) == ESP_OK;
    close_rw(h);
    ESP_LOGI(TAG, "%s %d: %s", flag, (int)on, ok ? "ok" : "FAILED");
    return ok;
}

bool config_load_stay_wl(void) { return flag_load(K_STAY_WL); }
bool config_save_stay_wl(bool on) { return flag_save(K_STAY_WL, on); }
bool config_load_boot_usb(void) { return flag_load(K_BOOT_USB); }
bool config_save_boot_usb(bool on) { return flag_save(K_BOOT_USB, on); }

uint8_t config_load_wl_effective(void)
{
    config_net_info_t info;
    config_net_info(&info);                       // mode: 1 only over whole Wi-Fi settings, 2 only over whole phone settings, else 0
    if (info.mode == 1) return 1;
    if (info.mode == 2) {
        // The phone's key is read here (and wiped) so that a marker over an all-zero key is not a target either.
        config_ble_t b;
        const bool ok = config_ble_load(&b);
        scrub(&b, sizeof b);
        return ok ? 2 : 0;
    }
    return 0;
}

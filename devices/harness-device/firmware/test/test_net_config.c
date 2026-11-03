// The dial's Wi-Fi settings in NVS (config_store.c config_net_*; docs/specs/cable-wifi-link.md section 3), against an in-memory
// NVS (host_nvs/) that can be told to fail after N more writes: a power cut at that point.
//
// What is held to: every field is checked before anything is written; wl_ver is the commit marker and is gone before a rewrite
// begins and set last, so a cut between keys leaves the dial unprovisioned and never half of one network and half of another;
// clearing takes wl_ver first; config_clear_all takes everything; and no value is ever logged.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "nvs.h"

// ── the log, captured: lengths may appear in it, values may not ──
static char s_log[8192];
static void capture(const char *tag, const char *fmt, va_list ap)
{
    size_t used = strlen(s_log);
    used += (size_t)snprintf(s_log + used, sizeof s_log - used, "%s: ", tag);
    if (used < sizeof s_log) vsnprintf(s_log + used, sizeof s_log - used, fmt, ap);
    strncat(s_log, "\n", sizeof s_log - strlen(s_log) - 1);
}
void host_log_info(const char *tag, const char *fmt, ...) { va_list ap; va_start(ap, fmt); capture(tag, fmt, ap); va_end(ap); }
void host_log_warn(const char *tag, const char *fmt, ...) { va_list ap; va_start(ap, fmt); capture(tag, fmt, ap); va_end(ap); }

extern void host_nvs_reset(void);
extern void host_nvs_fail_after(int mutations);
extern int host_nvs_mutations(void);
extern int host_nvs_count(void);
extern int host_nvs_has(const char *key);
extern size_t host_nvs_size(const char *key);
extern const void *host_nvs_raw(const char *key);
extern void host_nvs_forbid_read(const char *key);

#define SSID "home-net"
#define PASS "correct horse battery"
#define HOST "192.168.1.20"
static const uint8_t KEY[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
static const uint8_t KEY2[32] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
static const uint8_t ZERO_KEY[32] = {0};
static const char *const WL_KEYS[] = {"wl_ssid", "wl_pass", "wl_host", "wl_port", "wl_key", "wl_ver", "wl_mode"};

static void fresh(void) { host_nvs_reset(); s_log[0] = 0; }
static int wl_keys_held(void) { int n = 0; for (unsigned i = 0; i < sizeof WL_KEYS / sizeof WL_KEYS[0]; i++) n += host_nvs_has(WL_KEYS[i]); return n; }
static config_net_err_t set_good(void) { return config_net_set(SSID, PASS, HOST, 18480, KEY, 1); }
static bool loads(void) { config_net_t n; return config_net_load(&n); }

static void test_validation_table(void)
{
    struct { const char *ssid, *pass, *host; long port; const uint8_t *key; long mode; config_net_err_t want; const char *why; } t[] = {
        {SSID, PASS, HOST, 18480, KEY, 1, CFG_NET_OK, "all good"},
        {SSID, "", HOST, 18480, KEY, 0, CFG_NET_OK, "an open network has an empty password"},
        {"a", "12345678", "10.0.0.1", 1024, KEY, 0, CFG_NET_OK, "the smallest of each"},
        {"12345678901234567890123456789012", "123456789012345678901234567890123456789012345678901234567890123", "223.255.255.254", 65535, KEY, 1, CFG_NET_OK, "the largest of each"},
        {SSID, PASS, "126.255.255.255", 18480, KEY, 1, CFG_NET_OK, "just below loopback"},
        {SSID, PASS, "128.0.0.1", 18480, KEY, 1, CFG_NET_OK, "just above loopback"},
        {SSID, PASS, "240.0.0.1", 18480, KEY, 1, CFG_NET_OK, "above multicast"},
        {SSID, PASS, "0.0.0.1", 18480, KEY, 1, CFG_NET_OK, "only 0.0.0.0 itself is refused"},
        {SSID, PASS, "0.0.0.0", 18480, KEY, 1, CFG_NET_E_HOST, "0.0.0.0"},
        {SSID, PASS, "255.255.255.255", 18480, KEY, 1, CFG_NET_E_HOST, "broadcast"},
        {SSID, PASS, "127.0.0.1", 18480, KEY, 1, CFG_NET_E_HOST, "loopback"},
        {SSID, PASS, "127.255.255.254", 18480, KEY, 1, CFG_NET_E_HOST, "loopback, the top of the block"},
        {SSID, PASS, "224.0.0.1", 18480, KEY, 1, CFG_NET_E_HOST, "multicast"},
        {SSID, PASS, "239.255.255.255", 18480, KEY, 1, CFG_NET_E_HOST, "multicast, the top of the block"},
        {SSID, PASS, HOST, 18480, ZERO_KEY, 1, CFG_NET_E_KEY, "an all-zero key"},
        {NULL, PASS, HOST, 18480, KEY, 1, CFG_NET_E_SSID, "no ssid"},
        {"", PASS, HOST, 18480, KEY, 1, CFG_NET_E_SSID, "empty ssid"},
        {"123456789012345678901234567890123", PASS, HOST, 18480, KEY, 1, CFG_NET_E_SSID, "33-byte ssid"},
        {"bad\nname", PASS, HOST, 18480, KEY, 1, CFG_NET_E_SSID, "ssid with a control character"},
        {"bad\x7f", PASS, HOST, 18480, KEY, 1, CFG_NET_E_SSID, "ssid with DEL"},
        {SSID, NULL, HOST, 18480, KEY, 1, CFG_NET_E_PASS, "no password field"},
        {SSID, "1234567", HOST, 18480, KEY, 1, CFG_NET_E_PASS, "7-byte password"},
        {SSID, "1234567890123456789012345678901234567890123456789012345678901234", HOST, 18480, KEY, 1, CFG_NET_E_PASS, "64-byte password"},
        {SSID, "pass\tword", HOST, 18480, KEY, 1, CFG_NET_E_PASS, "password with a tab"},
        {SSID, PASS, NULL, 18480, KEY, 1, CFG_NET_E_HOST, "no host"},
        {SSID, PASS, "", 18480, KEY, 1, CFG_NET_E_HOST, "empty host"},
        {SSID, PASS, "192.168.1", 18480, KEY, 1, CFG_NET_E_HOST, "three parts"},
        {SSID, PASS, "192.168.1.1.1", 18480, KEY, 1, CFG_NET_E_HOST, "five parts"},
        {SSID, PASS, "192.168.1.256", 18480, KEY, 1, CFG_NET_E_HOST, "octet 256"},
        {SSID, PASS, "192.168.01.1", 18480, KEY, 1, CFG_NET_E_HOST, "leading zero"},
        {SSID, PASS, "192.168.1.", 18480, KEY, 1, CFG_NET_E_HOST, "trailing dot"},
        {SSID, PASS, ".192.168.1", 18480, KEY, 1, CFG_NET_E_HOST, "leading dot"},
        {SSID, PASS, "192.168..1", 18480, KEY, 1, CFG_NET_E_HOST, "empty part"},
        {SSID, PASS, "host.local", 18480, KEY, 1, CFG_NET_E_HOST, "a name"},
        {SSID, PASS, " 192.168.1.20", 18480, KEY, 1, CFG_NET_E_HOST, "a space"},
        {SSID, PASS, "1000.1.1.1", 18480, KEY, 1, CFG_NET_E_HOST, "four digits"},
        {SSID, PASS, "::1", 18480, KEY, 1, CFG_NET_E_HOST, "IPv6"},
        {SSID, PASS, "192.168.001.1", 18480, KEY, 1, CFG_NET_E_HOST, "two leading zeros"},
        {SSID, PASS, HOST, 1023, KEY, 1, CFG_NET_E_PORT, "port 1023"},
        {SSID, PASS, HOST, 65536, KEY, 1, CFG_NET_E_PORT, "port 65536"},
        {SSID, PASS, HOST, 0, KEY, 1, CFG_NET_E_PORT, "port 0"},
        {SSID, PASS, HOST, -1, KEY, 1, CFG_NET_E_PORT, "no port"},
        {SSID, PASS, HOST, 18480, NULL, 1, CFG_NET_E_KEY, "no key"},
        {SSID, PASS, HOST, 18480, KEY, 2, CFG_NET_E_MODE, "mode 2"},
        {SSID, PASS, HOST, 18480, KEY, -1, CFG_NET_E_MODE, "no mode"},
        // the first bad field is the one named, in the order ssid, pass, host, port, key, mode
        {"", "1", "x", 1, NULL, 7, CFG_NET_E_SSID, "everything bad: ssid first"},
        {SSID, "1", "x", 1, NULL, 7, CFG_NET_E_PASS, "all but ssid bad: pass first"},
        {SSID, PASS, "x", 1, NULL, 7, CFG_NET_E_HOST, "host before port"},
        {SSID, PASS, HOST, 1, NULL, 7, CFG_NET_E_PORT, "port before key"},
        {SSID, PASS, HOST, 18480, NULL, 7, CFG_NET_E_KEY, "key before mode"},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        fresh();
        config_net_err_t got = config_net_set(t[i].ssid, t[i].pass, t[i].host, t[i].port, t[i].key, t[i].mode);
        if (got != t[i].want) { fprintf(stderr, "case %u (%s): got %d want %d\n", i, t[i].why, (int)got, (int)t[i].want); assert(0); }
        if (t[i].want != CFG_NET_OK) {
            // a refusal touches nothing
            assert(host_nvs_count() == 0);
            assert(host_nvs_mutations() == 0);
            assert(!loads());
        } else {
            assert(loads());
        }
    }
    assert(!strcmp(config_net_err_name(CFG_NET_E_SSID), "ssid"));
    assert(!strcmp(config_net_err_name(CFG_NET_E_PASS), "pass"));
    assert(!strcmp(config_net_err_name(CFG_NET_E_HOST), "host"));
    assert(!strcmp(config_net_err_name(CFG_NET_E_PORT), "port"));
    assert(!strcmp(config_net_err_name(CFG_NET_E_KEY), "key"));
    assert(!strcmp(config_net_err_name(CFG_NET_E_MODE), "mode"));
    assert(!strcmp(config_net_err_name(CFG_NET_E_NOT_PROVISIONED), "not_provisioned"));
    assert(!strcmp(config_net_err_name(CFG_NET_E_STORAGE), "storage"));
    assert(!strcmp(config_net_err_name(CFG_NET_OK), "ok"));
}

static void test_round_trip_and_shapes(void)
{
    fresh();
    assert(!loads());
    config_net_info_t info;
    config_net_info(&info);
    assert(!info.saved);
    assert(set_good() == CFG_NET_OK);
    config_net_t n;
    assert(config_net_load(&n));
    assert(!strcmp(n.ssid, SSID) && !strcmp(n.pass, PASS) && !strcmp(n.host, HOST) && n.port == 18480 && n.mode == 1);
    assert(!memcmp(n.key, KEY, 32));
    config_net_info(&info);
    assert(info.saved && !strcmp(info.ssid, SSID) && !strcmp(info.host, HOST) && info.port == 18480 && info.mode == 1);
    // the stored shapes, as the spec's 5.2 table gives them
    assert(host_nvs_size("wl_ssid") == strlen(SSID) + 1 && !strcmp(host_nvs_raw("wl_ssid"), SSID));
    assert(host_nvs_size("wl_pass") == strlen(PASS) + 1);
    assert(host_nvs_size("wl_host") == strlen(HOST) + 1);
    assert(host_nvs_size("wl_port") == 2);
    assert(host_nvs_size("wl_key") == 32 && !memcmp(host_nvs_raw("wl_key"), KEY, 32));
    assert(host_nvs_size("wl_ver") == 1 && *(const uint8_t *)host_nvs_raw("wl_ver") == 1);
    assert(host_nvs_size("wl_mode") == 1 && *(const uint8_t *)host_nvs_raw("wl_mode") == 1);
    // an open network is held as an empty password, and still loads
    fresh();
    assert(config_net_set(SSID, "", HOST, 18480, KEY, 0) == CFG_NET_OK);
    assert(config_net_load(&n) && n.pass[0] == 0 && n.mode == 0);
}

static void test_power_cut_between_keys(void)
{
    // Count the writes of a whole set on a store that holds nothing, then cut the power after each of them in turn.
    fresh();
    host_nvs_fail_after(-1);
    assert(set_good() == CFG_NET_OK);
    const int total = host_nvs_mutations();
    assert(total >= 8);
    for (int cut = 0; cut < total; cut++) {
        fresh();
        host_nvs_fail_after(cut);
        config_net_err_t got = set_good();
        assert(got == CFG_NET_E_STORAGE);
        // a cut anywhere before the last write leaves the dial unprovisioned; the last write is the commit after wl_ver, by which
        // time the settings are whole
        if (cut < total - 1) {
            assert(!host_nvs_has("wl_ver"));
            assert(!loads());
            config_net_info_t info;
            config_net_info(&info);
            assert(!info.saved);
        } else {
            assert(loads());
        }
    }
    // and over a dial that already holds a network: a cut leaves it unprovisioned, never half of each
    for (int cut = 0; cut < total; cut++) {
        fresh();
        assert(config_net_set("old-net", "old-password", "10.0.0.2", 2000, KEY2, 0) == CFG_NET_OK);
        host_nvs_fail_after(cut);
        config_net_err_t got = config_net_set(SSID, PASS, HOST, 18480, KEY, 1);
        assert(got == CFG_NET_E_STORAGE);
        config_net_t n;
        if (cut == 0) assert(config_net_load(&n) && !strcmp(n.ssid, "old-net") && !memcmp(n.key, KEY2, 32));   // nothing was touched: the old one stands
        else if (cut < total - 1) assert(!config_net_load(&n));                                              // the marker is gone: neither
        else assert(config_net_load(&n) && !strcmp(n.ssid, SSID) && !memcmp(n.key, KEY, 32));                  // whole, the new one
    }
}

/** A change to a held configuration, cut after each write in turn: the dial is unprovisioned, or has the old one, or (the last commit only) the new one. */
static void power_cut_in_a_change(const char *what, config_net_err_t (*change)(void), bool (*is_old)(const config_net_t *), bool (*is_new)(const config_net_t *))
{
    fresh();
    assert(config_net_set("old-net", "old-password", "10.0.0.2", 2000, KEY2, 0) == CFG_NET_OK);
    host_nvs_fail_after(-1);
    assert(change() == CFG_NET_OK);
    const int total = host_nvs_mutations();
    assert(total >= 6);   // marker off + commit, the write(s), commit, marker on + commit
    for (int cut = 0; cut < total; cut++) {
        fresh();
        assert(config_net_set("old-net", "old-password", "10.0.0.2", 2000, KEY2, 0) == CFG_NET_OK);
        host_nvs_fail_after(cut);
        config_net_err_t got = change();
        config_net_t n;
        if (got != CFG_NET_E_STORAGE) { fprintf(stderr, "%s cut %d: got %d\n", what, cut, (int)got); assert(0); }
        if (cut == 0) assert(config_net_load(&n) && is_old(&n));            // nothing was touched: the old one stands
        else if (cut < total - 1) assert(!config_net_load(&n));              // the marker is gone: unprovisioned, never half changed
        else assert(config_net_load(&n) && is_new(&n));                      // whole, the new one
    }
}
static config_net_err_t change_key(void) { return config_net_set_key(KEY); }
static bool old_key(const config_net_t *n) { return !memcmp(n->key, KEY2, 32) && !strcmp(n->ssid, "old-net"); }
static bool new_key(const config_net_t *n) { return !memcmp(n->key, KEY, 32) && !strcmp(n->ssid, "old-net"); }
static config_net_err_t change_host(void) { return config_net_set_host("10.9.8.7", 19000); }
static bool old_host(const config_net_t *n) { return !strcmp(n->host, "10.0.0.2") && n->port == 2000; }
static bool new_host(const config_net_t *n) { return !strcmp(n->host, "10.9.8.7") && n->port == 19000 && !memcmp(n->key, KEY2, 32); }
static config_net_err_t change_mode(void) { return config_net_set_mode(1); }
static bool old_mode(const config_net_t *n) { return n->mode == 0; }
static bool new_mode(const config_net_t *n) { return n->mode == 1 && !strcmp(n->ssid, "old-net"); }

static void test_power_cut_in_a_change(void)
{
    power_cut_in_a_change("key", change_key, old_key, new_key);
    power_cut_in_a_change("host", change_host, old_host, new_host);
    power_cut_in_a_change("mode", change_mode, old_mode, new_mode);
    // an all-zero key is no change at all
    fresh();
    assert(set_good() == CFG_NET_OK);
    host_nvs_fail_after(-1);
    const int before = host_nvs_mutations();
    assert(config_net_set_key(ZERO_KEY) == CFG_NET_E_KEY && host_nvs_mutations() == before);
    config_net_t n;
    assert(config_net_load(&n) && !memcmp(n.key, KEY, 32));
}

static void test_info_never_reads_the_secrets(void)
{
    fresh();
    assert(set_good() == CFG_NET_OK);
    host_nvs_forbid_read("wl_pass");
    host_nvs_forbid_read("wl_key");
    config_net_info_t info;
    config_net_info(&info);   // would trap if it read either into a buffer
    assert(info.saved && !strcmp(info.ssid, SSID) && !strcmp(info.host, HOST) && info.port == 18480 && info.mode == 1);
    // an open network has a password of length 0, a key of 31 bytes is not a key, a password of 7 or 65 bytes is not one: all by size alone
    fresh();
    assert(config_net_set(SSID, "", HOST, 18480, KEY, 0) == CFG_NET_OK);
    host_nvs_forbid_read("wl_pass");
    host_nvs_forbid_read("wl_key");
    config_net_info(&info);
    assert(info.saved && info.mode == 0);
    nvs_handle_t h;
    assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK);
    assert(nvs_set_blob(h, "wl_key", KEY, 31) == ESP_OK);
    config_net_info(&info);
    assert(!info.saved && !info.ssid[0] && !info.host[0] && !info.port);
    assert(nvs_set_blob(h, "wl_key", KEY, 32) == ESP_OK);
    assert(nvs_set_str(h, "wl_pass", "1234567") == ESP_OK);
    config_net_info(&info);
    assert(!info.saved);
    assert(nvs_set_str(h, "wl_pass", "1234567890123456789012345678901234567890123456789012345678901234") == ESP_OK);
    config_net_info(&info);
    assert(!info.saved);
    assert(nvs_set_str(h, "wl_pass", "12345678") == ESP_OK);
    config_net_info(&info);
    assert(info.saved);
}

static void test_commit_marker_is_last_and_first_out(void)
{
    // wl_ver is the last key set: when it is absent after a failure, nothing else decides; when it is there, every other key is.
    fresh();
    assert(set_good() == CFG_NET_OK);
    for (unsigned i = 0; i < sizeof WL_KEYS / sizeof WL_KEYS[0]; i++) assert(host_nvs_has(WL_KEYS[i]));
    // a marker with a key missing is not a configuration
    const char *needed[] = {"wl_ssid", "wl_pass", "wl_host", "wl_port", "wl_key"};
    for (unsigned i = 0; i < sizeof needed / sizeof needed[0]; i++) {
        fresh();
        assert(set_good() == CFG_NET_OK);
        nvs_handle_t h;
        assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK);
        assert(nvs_erase_key(h, needed[i]) == ESP_OK);
        config_net_t n;
        assert(!config_net_load(&n));
        config_net_info_t info;
        config_net_info(&info);
        assert(!info.saved);
    }
    // a marker of another version is not one this firmware reads
    fresh();
    assert(set_good() == CFG_NET_OK);
    nvs_handle_t h;
    assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK);
    assert(nvs_set_u8(h, "wl_ver", 2) == ESP_OK);
    assert(!loads());
    // and a stored key of the wrong size, or a host that is not one, is not a configuration either
    fresh();
    assert(set_good() == CFG_NET_OK);
    assert(nvs_set_blob(h, "wl_key", KEY, 31) == ESP_OK);
    assert(!loads());
    fresh();
    assert(set_good() == CFG_NET_OK);
    assert(nvs_set_str(h, "wl_host", "not an address") == ESP_OK);
    assert(!loads());
    fresh();
    assert(set_good() == CFG_NET_OK);
    assert(nvs_set_u16(h, "wl_port", 80) == ESP_OK);
    assert(!loads());
    fresh();
    assert(set_good() == CFG_NET_OK);
    assert(nvs_set_str(h, "wl_ssid", "") == ESP_OK);
    assert(!loads());
}

static void test_clear(void)
{
    fresh();
    assert(set_good() == CFG_NET_OK);
    nvs_handle_t h;
    assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK);
    assert(nvs_set_u8(h, "bright", 100) == ESP_OK);   // not a Wi-Fi key
    assert(config_net_clear() == CFG_NET_OK);
    assert(wl_keys_held() == 0);
    assert(!loads());
    assert(host_nvs_has("bright"));                    // and the rest of the dial's settings stay
    // clearing what is not there is fine
    assert(config_net_clear() == CFG_NET_OK);
    // wl_ver goes first: a cut after the first erase leaves the dial unprovisioned whatever else is still held
    fresh();
    assert(set_good() == CFG_NET_OK);
    host_nvs_fail_after(-1);
    assert(config_net_clear() == CFG_NET_OK);
    const int total = host_nvs_mutations();
    for (int cut = 1; cut < total; cut++) {
        fresh();
        assert(set_good() == CFG_NET_OK);
        host_nvs_fail_after(cut);
        assert(config_net_clear() == CFG_NET_E_STORAGE);
        assert(!host_nvs_has("wl_ver"));
        assert(!loads());
    }
    // a cut before anything was done leaves the settings whole
    fresh();
    assert(set_good() == CFG_NET_OK);
    host_nvs_fail_after(0);
    assert(config_net_clear() == CFG_NET_E_STORAGE);
    assert(loads());
}

static void test_changes_need_a_configuration(void)
{
    fresh();
    assert(config_net_set_key(KEY) == CFG_NET_E_NOT_PROVISIONED);
    assert(config_net_set_host(HOST, 18480) == CFG_NET_E_NOT_PROVISIONED);
    assert(config_net_set_mode(1) == CFG_NET_E_NOT_PROVISIONED);
    assert(host_nvs_count() == 0);
    assert(set_good() == CFG_NET_OK);
    // key
    assert(config_net_set_key(NULL) == CFG_NET_E_KEY);
    assert(config_net_set_key(KEY2) == CFG_NET_OK);
    config_net_t n;
    assert(config_net_load(&n) && !memcmp(n.key, KEY2, 32) && !strcmp(n.ssid, SSID) && !strcmp(n.pass, PASS));
    // host and port
    assert(config_net_set_host("nope", 18480) == CFG_NET_E_HOST);
    assert(config_net_set_host(HOST, 80) == CFG_NET_E_PORT);
    assert(config_net_set_host("10.1.2.3", 19000) == CFG_NET_OK);
    assert(config_net_load(&n) && !strcmp(n.host, "10.1.2.3") && n.port == 19000 && !memcmp(n.key, KEY2, 32));
    // mode
    assert(config_net_set_mode(2) == CFG_NET_E_NOT_PROVISIONED);   // B6: 2 is the phone, and these are Wi-Fi settings (test_ble_config.c holds the rest)
    assert(config_net_set_mode(3) == CFG_NET_E_MODE);
    assert(config_net_set_mode(-1) == CFG_NET_E_MODE);
    assert(config_net_set_mode(0) == CFG_NET_OK);
    assert(config_net_load(&n) && n.mode == 0);
    assert(config_net_set_mode(1) == CFG_NET_OK);
    assert(config_net_load(&n) && n.mode == 1);
    // a failed write is reported, and leaves the whole it was
    host_nvs_fail_after(0);
    assert(config_net_set_key(KEY) == CFG_NET_E_STORAGE);
    assert(config_net_set_host(HOST, 18480) == CFG_NET_E_STORAGE);
    assert(config_net_set_mode(0) == CFG_NET_E_STORAGE);
    host_nvs_fail_after(-1);
    assert(config_net_load(&n) && !memcmp(n.key, KEY2, 32) && n.mode == 1 && !strcmp(n.host, "10.1.2.3"));
    // after a clear they need a configuration again
    assert(config_net_clear() == CFG_NET_OK);
    assert(config_net_set_key(KEY) == CFG_NET_E_NOT_PROVISIONED);
}

static void test_factory_reset_takes_everything(void)
{
    fresh();
    assert(set_good() == CFG_NET_OK);
    nvs_handle_t h;
    assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK);
    assert(nvs_set_u8(h, "bright", 100) == ESP_OK);
    assert(nvs_set_u8(h, "stay_wl", 1) == ESP_OK);
    assert(nvs_set_u8(h, "boot_usb", 1) == ESP_OK);
    assert(config_clear_all());
    assert(host_nvs_count() == 0);
    assert(wl_keys_held() == 0);
    assert(!loads());
}

static void test_key_decode(void)
{
    uint8_t out[32];
    assert(config_net_key_decode("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=", out));
    assert(!memcmp(out, KEY, 32));
    assert(config_net_key_decode("CQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQk=", out));
    assert(!memcmp(out, KEY2, 32));
    const char *bad[] = {
        "",
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8",       // no padding
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8==",     // too long
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHg==",      // 31 bytes
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh9=",      // trailing bits set: not canonical
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh-=",      // url-safe alphabet
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh_=",
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8\n",     // newline in place of padding
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8= ",     // trailing space
        " AECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=",      // space inside
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwd=h8=",      // padding inside
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8*",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (config_net_key_decode(bad[i], out)) { fprintf(stderr, "accepted: %s\n", bad[i]); assert(0); }
    }
    assert(!config_net_key_decode(NULL, out));
}

static void test_nothing_is_logged_but_lengths(void)
{
    fresh();
    assert(set_good() == CFG_NET_OK);
    assert(config_net_set_key(KEY2) == CFG_NET_OK);
    assert(config_net_set_host("10.9.8.7", 19000) == CFG_NET_OK);
    assert(config_net_set_mode(0) == CFG_NET_OK);
    assert(config_net_set("bad", PASS, HOST, 18480, KEY, 1) == CFG_NET_OK);
    assert(config_net_set(SSID, "short", HOST, 18480, KEY, 1) == CFG_NET_E_PASS);
    assert(config_net_clear() == CFG_NET_OK);
    host_nvs_fail_after(0);
    assert(set_good() == CFG_NET_E_STORAGE);
    assert(config_net_clear() == CFG_NET_E_STORAGE);
    const char *secrets[] = {SSID, "bad", PASS, "short", HOST, "10.9.8.7", "AAECAwQF", "\x01\x02\x03\x04", "\x09\x09\x09\x09", "home", "horse", "192.168"};
    for (unsigned i = 0; i < sizeof secrets / sizeof secrets[0]; i++)
        if (strstr(s_log, secrets[i])) { fprintf(stderr, "logged: %s\n--- log ---\n%s", secrets[i], s_log); assert(0); }
    // the lengths are there, so a bug report can say what was set
    assert(strstr(s_log, "ssid_len=8") && strstr(s_log, "pass_len=21") && strstr(s_log, "host_len=12"));
}

int main(void)
{
    test_validation_table();
    test_round_trip_and_shapes();
    test_power_cut_between_keys();
    test_power_cut_in_a_change();
    test_info_never_reads_the_secrets();
    test_commit_marker_is_last_and_first_out();
    test_clear();
    test_changes_need_a_configuration();
    test_factory_reset_takes_everything();
    test_key_decode();
    test_nothing_is_logged_but_lengths();
    puts("ok");
    return 0;
}

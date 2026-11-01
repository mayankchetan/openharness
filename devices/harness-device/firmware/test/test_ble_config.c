// The dial's phone-mode settings in NVS (config_store.c config_net_set_ble, config_ble_load, wl_mode 2; task B6 of the phone bridge), against the
// in-memory NVS of host_nvs/ that can be told to fail after N more writes: a power cut at that point.
//
// What is held to:
//   - the key-only form stores wl_key, then bl_ver LAST (the commit marker of the phone mode, like wl_ver for Wi-Fi): a cut anywhere leaves bl_ver
//     absent, so the dial holds no phone configuration and never starts BLE;
//   - a clear takes bl_ver first, then the key, with the Wi-Fi settings;
//   - wl_mode 2 is stored only with bl_ver and a key (CFG_NET_E_NOT_PROVISIONED otherwise), and reads as the target 2 only with them;
//     everything else that names 2 reads as "no target" (the dial boots USB), as before B6;
//   - the full Wi-Fi form still refuses mode 2; Wi-Fi settings and the phone configuration share one key, and neither marker is half-written;
//   - no value is logged.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "nvs.h"

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

#define SSID "home-net"
#define PASS "correct horse battery"
#define HOST "192.168.1.20"
static const uint8_t KEY[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
static const uint8_t KEY2[32] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
static const uint8_t ZERO_KEY[32] = {0};

static void fresh(void) { host_nvs_reset(); s_log[0] = 0; }
static void put_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK);
    assert(nvs_set_u8(h, key, v) == ESP_OK);
}
static void erase(const char *key)
{
    nvs_handle_t h;
    assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK);
    nvs_erase_key(h, key);
}
static bool ble_loads(uint8_t key_out[32])
{
    config_ble_t b;
    const bool ok = config_ble_load(&b);
    if (ok && key_out) memcpy(key_out, b.key, 32);
    return ok;
}
static void provision_wifi(long mode) { assert(config_net_set(SSID, PASS, HOST, 18480, KEY, mode) == CFG_NET_OK); }
static config_net_info_t info(void) { config_net_info_t i; config_net_info(&i); return i; }

static void test_key_only_form_round_trip(void)
{
    fresh();
    assert(!ble_loads(NULL) && config_load_wl_effective() == 0);
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    uint8_t k[32];
    assert(ble_loads(k) && !memcmp(k, KEY, 32));
    assert(host_nvs_has("bl_ver") && host_nvs_has("wl_key") && host_nvs_size("wl_key") == 32);
    assert(*(const uint8_t *)host_nvs_raw("bl_ver") == 1);
    // nothing of the Wi-Fi form was written: this is not a Wi-Fi configuration
    assert(!host_nvs_has("wl_ssid") && !host_nvs_has("wl_pass") && !host_nvs_has("wl_host") && !host_nvs_has("wl_port") && !host_nvs_has("wl_ver"));
    config_net_t n;
    assert(!config_net_load(&n));
    assert(!info().saved && info().ble);
    // it does not make a target by itself: the mode is still unset
    assert(config_load_wl_effective() == 0);
    // a second set replaces the key
    assert(config_net_set_ble(KEY2) == CFG_NET_OK);
    assert(ble_loads(k) && !memcmp(k, KEY2, 32));
}

static void test_the_key_is_checked_before_anything_is_written(void)
{
    fresh();
    assert(config_net_set_ble(NULL) == CFG_NET_E_KEY);
    assert(config_net_set_ble(ZERO_KEY) == CFG_NET_E_KEY);
    assert(host_nvs_count() == 0 && host_nvs_mutations() == 0);
    assert(!ble_loads(NULL));
}

static void test_power_cut_leaves_the_marker_absent(void)
{
    fresh();
    host_nvs_fail_after(-1);
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    const int total = host_nvs_mutations();
    assert(total >= 3);
    for (int cut = 0; cut < total; cut++) {
        fresh();
        host_nvs_fail_after(cut);
        const config_net_err_t err = config_net_set_ble(KEY);
        host_nvs_fail_after(-1);
        assert(err == CFG_NET_E_STORAGE);
        // a cut anywhere before the last write leaves no marker, so no configuration; the last write is the commit after bl_ver, by which time
        // the settings are whole (as for Wi-Fi: test_net_config.c)
        if (cut < total - 1) {
            assert(!ble_loads(NULL));
            assert(!host_nvs_has("bl_ver"));
        } else {
            assert(ble_loads(NULL));
        }
        assert(config_load_wl_effective() == 0);        // and no mode was set either way
    }
    // a cut in a CHANGE (an existing configuration, a new key) leaves none, never the old key with a half-written new one
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    put_u8("wl_mode", 2);
    assert(config_load_wl_effective() == 2);
    host_nvs_fail_after(-1);
    const int before = host_nvs_mutations();
    assert(config_net_set_ble(KEY2) == CFG_NET_OK);
    const int steps = host_nvs_mutations() - before;
    for (int cut = 0; cut < steps; cut++) {
        fresh();
        assert(config_net_set_ble(KEY) == CFG_NET_OK);
        put_u8("wl_mode", 2);
        host_nvs_fail_after(cut);
        (void)config_net_set_ble(KEY2);
        host_nvs_fail_after(-1);
        // the same contract as a Wi-Fi change (test_net_config.c): nothing touched (cut 0) is the old one; a cut after the marker came off is no
        // configuration and so no target; only the last commit gives the new one
        uint8_t k[32];
        if (cut == 0) assert(ble_loads(k) && !memcmp(k, KEY, 32) && config_load_wl_effective() == 2);
        else if (cut < steps - 1) assert(!ble_loads(NULL) && config_load_wl_effective() == 0);
        else assert(ble_loads(k) && !memcmp(k, KEY2, 32));
    }
}

static void test_the_marker_is_written_last(void)
{
    // with a cut just before the last write, the key is there and the marker is not
    fresh();
    host_nvs_fail_after(-1);
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    const int total = host_nvs_mutations();
    fresh();
    host_nvs_fail_after(total - 2);                  // the write of bl_ver itself is the one that fails
    assert(config_net_set_ble(KEY) == CFG_NET_E_STORAGE);
    host_nvs_fail_after(-1);
    assert(host_nvs_has("wl_key") && !host_nvs_has("bl_ver") && !ble_loads(NULL));
}

static void test_mode_2_needs_the_marker_and_a_key(void)
{
    fresh();
    assert(config_net_set_mode(2) == CFG_NET_E_NOT_PROVISIONED);             // nothing at all
    assert(host_nvs_count() == 0);
    provision_wifi(1);
    assert(config_net_set_mode(2) == CFG_NET_E_NOT_PROVISIONED);             // a Wi-Fi configuration is not a phone configuration
    assert(config_load_wl_effective() == 1 && info().mode == 1);
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    assert(config_net_set_mode(2) == CFG_NET_OK);
    assert(config_load_wl_effective() == 2 && info().mode == 2);
    assert(config_net_set_mode(3) == CFG_NET_E_MODE && config_net_set_mode(-1) == CFG_NET_E_MODE);
    assert(config_net_check_mode(0) == CFG_NET_OK && config_net_check_mode(1) == CFG_NET_OK && config_net_check_mode(2) == CFG_NET_OK);
    assert(config_net_check_mode(3) == CFG_NET_E_MODE && config_net_check_mode(-1) == CFG_NET_E_MODE);
    // back to Wi-Fi and to USB
    assert(config_net_set_mode(1) == CFG_NET_OK && config_load_wl_effective() == 1);
    assert(config_net_set_mode(2) == CFG_NET_OK && config_net_set_mode(0) == CFG_NET_OK && config_load_wl_effective() == 0);

    // a phone-only dial (no Wi-Fi at all)
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    assert(config_net_set_mode(1) == CFG_NET_E_NOT_PROVISIONED);             // Wi-Fi has no configuration
    assert(config_net_set_mode(2) == CFG_NET_OK);
    assert(config_load_wl_effective() == 2 && info().mode == 2 && !info().saved && info().ble);
    assert(config_net_set_mode(0) == CFG_NET_OK && config_load_wl_effective() == 0 && info().mode == 0);
    // the marker is never left off by a mode change
    assert(host_nvs_has("bl_ver"));
}

static void test_the_full_form_still_refuses_mode_2(void)
{
    fresh();
    assert(config_net_set(SSID, PASS, HOST, 18480, KEY, 2) == CFG_NET_E_MODE);
    assert(host_nvs_count() == 0);
    assert(config_net_check(SSID, PASS, HOST, 18480, KEY, 2) == CFG_NET_E_MODE);
}

static void test_wl_effective_for_every_stored_byte(void)
{
    // phone configuration whole: 2 is the target, nothing else is
    static const struct { uint8_t byte; uint8_t want; } t[] = {{0, 0}, {1, 0}, {2, 2}, {3, 0}, {7, 0}, {128, 0}, {255, 0}};
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        fresh();
        assert(config_net_set_ble(KEY) == CFG_NET_OK);
        put_u8("wl_mode", t[i].byte);
        if (config_load_wl_effective() != t[i].want) { fprintf(stderr, "ble byte %u: got %u want %u\n", t[i].byte, config_load_wl_effective(), t[i].want); assert(0); }
    }
    // Wi-Fi configuration whole, no phone marker: byte 2 reads 0 (this is the old rule)
    fresh();
    provision_wifi(1);
    put_u8("wl_mode", 2);
    assert(config_load_wl_effective() == 0 && info().mode == 0);
    // the marker missing, or corrupt, or the key missing: no target
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    put_u8("wl_mode", 2);
    erase("bl_ver");
    assert(config_load_wl_effective() == 0);
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    put_u8("wl_mode", 2);
    put_u8("bl_ver", 7);
    assert(config_load_wl_effective() == 0 && !ble_loads(NULL));
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    put_u8("wl_mode", 2);
    erase("wl_key");
    assert(config_load_wl_effective() == 0 && !ble_loads(NULL));
    // a key of the wrong size under a good marker
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    put_u8("wl_mode", 2);
    {
        nvs_handle_t h;
        assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK);
        assert(nvs_set_blob(h, "wl_key", KEY, 16) == ESP_OK);
    }
    assert(config_load_wl_effective() == 0 && !ble_loads(NULL));
    // an all-zero key under a good marker is not a key
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    put_u8("wl_mode", 2);
    {
        nvs_handle_t h;
        assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK);
        assert(nvs_set_blob(h, "wl_key", ZERO_KEY, 32) == ESP_OK);
    }
    assert(config_load_wl_effective() == 0 && !ble_loads(NULL));
}

static void test_both_configurations_share_one_key(void)
{
    // phone configuration first, then Wi-Fi: the Wi-Fi form's key replaces it; both stay whole (one key per dial)
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    provision_wifi(1);
    uint8_t k[32];
    assert(ble_loads(k) && !memcmp(k, KEY, 32) && info().saved && info().ble);
    // Wi-Fi first, then the key-only form: the Wi-Fi settings stay whole and use the new key
    fresh();
    provision_wifi(1);
    assert(config_net_set_ble(KEY2) == CFG_NET_OK);
    config_net_t n;
    assert(config_net_load(&n) && !memcmp(n.key, KEY2, 32) && !strcmp(n.ssid, SSID) && n.mode == 1);
    assert(ble_loads(k) && !memcmp(k, KEY2, 32));
    assert(config_load_wl_effective() == 1);                      // the mode did not move
    assert(host_nvs_has("wl_ver") && host_nvs_has("bl_ver"));
    // a cut in that second step never leaves the Wi-Fi settings half changed: wl_ver comes back only with the whole configuration
    fresh();
    provision_wifi(1);
    host_nvs_fail_after(-1);
    const int before = host_nvs_mutations();
    assert(config_net_set_ble(KEY2) == CFG_NET_OK);
    const int steps = host_nvs_mutations() - before;
    for (int cut = 0; cut < steps; cut++) {
        fresh();
        provision_wifi(1);
        host_nvs_fail_after(cut);
        (void)config_net_set_ble(KEY2);
        host_nvs_fail_after(-1);
        if (config_net_load(&n)) assert(!memcmp(n.key, KEY, 32) || !memcmp(n.key, KEY2, 32));
        if (!ble_loads(NULL)) assert(!host_nvs_has("bl_ver"));
    }
}

static void test_clear_and_factory_reset(void)
{
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK && config_net_set_mode(2) == CFG_NET_OK);
    assert(config_net_clear() == CFG_NET_OK);
    assert(!ble_loads(NULL) && config_load_wl_effective() == 0);
    assert(!host_nvs_has("bl_ver") && !host_nvs_has("wl_key") && !host_nvs_has("wl_mode"));
    assert(config_net_clear() == CFG_NET_OK);                                // fine when there is nothing to forget

    // a cut anywhere in the clear leaves no target, and bl_ver goes before wl_key
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK && config_net_set_mode(2) == CFG_NET_OK);
    host_nvs_fail_after(-1);
    const int before = host_nvs_mutations();
    assert(config_net_clear() == CFG_NET_OK);
    const int steps = host_nvs_mutations() - before;
    for (int cut = 0; cut < steps; cut++) {
        fresh();
        assert(config_net_set_ble(KEY) == CFG_NET_OK && config_net_set_mode(2) == CFG_NET_OK);
        host_nvs_fail_after(cut);
        (void)config_net_clear();
        host_nvs_fail_after(-1);
        assert(!(host_nvs_has("bl_ver") && !host_nvs_has("wl_key")));            // a marker is never left over a missing key
    }
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK && config_net_set_mode(2) == CFG_NET_OK);
    assert(config_clear_all());
    assert(!ble_loads(NULL) && config_load_wl_effective() == 0 && !host_nvs_has("bl_ver"));
}

static void test_nothing_is_logged_but_words(void)
{
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    assert(config_net_set_mode(2) == CFG_NET_OK);
    assert(config_net_set_ble(KEY2) == CFG_NET_OK);
    (void)config_net_set_ble(ZERO_KEY);
    assert(config_net_clear() == CFG_NET_OK);
    assert(strlen(s_log) > 0);
    // the key bytes, in any common spelling, are not in the log
    static const char *const bad[] = {"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=", "000102030405", "0x00, 0x01", "CQkJCQkJ", "090909090909"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) assert(!strstr(s_log, bad[i]));
}

int main(void)
{
    test_key_only_form_round_trip();
    test_the_key_is_checked_before_anything_is_written();
    test_power_cut_leaves_the_marker_absent();
    test_the_marker_is_written_last();
    test_mode_2_needs_the_marker_and_a_key();
    test_the_full_form_still_refuses_mode_2();
    test_wl_effective_for_every_stored_byte();
    test_both_configurations_share_one_key();
    test_clear_and_factory_reset();
    test_nothing_is_logged_but_words();
    puts("ok test_ble_config");
    return 0;
}

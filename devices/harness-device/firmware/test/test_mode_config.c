// The mode switch's three NVS values (config_store.c, task W15), against the in-memory NVS of host_nvs/.
//
// The rule they are held to is that no stored value can leave a dial unable to reach USB:
//   - stay_wl and boot_usb are true only for the byte 1. A corrupt byte (7, 255, 2) reads as false, which is the default behaviour (boot into
//     wl_mode, probe for a cable), never "stay in Wi-Fi for ever".
//   - the wireless target the mode switch sees (config_load_wl_effective) is Wi-Fi only when the Wi-Fi settings are WHOLE (the commit marker and
//     every key) and wl_mode is exactly 1 (2, for the phone, needs its own settings: test_ble_config.c). A wl_mode with no credentials, a corrupt wl_mode, wl_mode 2 over Wi-Fi settings,
//     a missing marker: all of them read as "no wireless target", and the mode switch then never leaves USB.
//   - forgetting the network (net.clear) and the factory reset take stay_wl with them, so a later provisioning cannot inherit an override that
//     never probes for a cable.
//   - boot_usb is written only when it changes (one flash write per mode change, none per boot).
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "nvs.h"

void host_log_info(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
void host_log_warn(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }

extern void host_nvs_reset(void);
extern void host_nvs_fail_after(int mutations);
extern int host_nvs_mutations(void);
extern int host_nvs_has(const char *key);
extern const void *host_nvs_raw(const char *key);

static const uint8_t KEY[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

static void fresh(void) { host_nvs_reset(); }
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
static void provision(long mode) { assert(config_net_set("home-net", "correct horse battery", "192.168.1.20", 18480, KEY, mode) == CFG_NET_OK); }

static void test_defaults(void)
{
    fresh();
    assert(!config_load_stay_wl());
    assert(!config_load_boot_usb());
    assert(config_load_wl_effective() == 0);
}

static void test_round_trip(void)
{
    fresh();
    assert(config_save_stay_wl(true));
    assert(config_load_stay_wl());
    assert(*(const uint8_t *)host_nvs_raw("stay_wl") == 1);
    assert(config_save_stay_wl(false));
    assert(!config_load_stay_wl());
    assert(config_save_boot_usb(true));
    assert(config_load_boot_usb());
    assert(config_save_boot_usb(false));
    assert(!config_load_boot_usb());
}

// A corrupt byte is false: the default behaviour, never a stuck override.
static void test_corrupt_bytes_read_false(void)
{
    static const uint8_t bad[] = {0, 2, 7, 128, 255};
    for (unsigned i = 0; i < sizeof bad; i++) {
        fresh();
        put_u8("stay_wl", bad[i]);
        put_u8("boot_usb", bad[i]);
        assert(!config_load_stay_wl());
        assert(!config_load_boot_usb());
    }
    fresh();
    put_u8("stay_wl", 1);
    put_u8("boot_usb", 1);
    assert(config_load_stay_wl() && config_load_boot_usb());
    // the wrong type under the key (a blob or a string where a byte is read) is also false
    fresh();
    nvs_handle_t h;
    assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK);
    assert(nvs_set_u16(h, "stay_wl", 0x0101) == ESP_OK);
    assert(nvs_set_str(h, "boot_usb", "1") == ESP_OK);
    assert(!config_load_stay_wl());
    assert(!config_load_boot_usb());
}

// boot_usb is written only when it changes; the write that is not needed is not made (flash wear).
static void test_boot_usb_written_only_on_change(void)
{
    fresh();
    host_nvs_fail_after(-1);
    assert(config_save_boot_usb(false));        // equal to the default: nothing to write
    assert(host_nvs_mutations() == 0);
    assert(config_save_boot_usb(true));
    const int first = host_nvs_mutations();
    assert(first > 0);
    assert(config_save_boot_usb(true));         // unchanged
    assert(host_nvs_mutations() == first);
    assert(config_save_boot_usb(false));
    assert(host_nvs_mutations() > first);
    // a store that refuses the write says so, and the old value stands
    fresh();
    assert(config_save_boot_usb(true));
    host_nvs_fail_after(0);
    assert(!config_save_boot_usb(false));
    host_nvs_fail_after(-1);
    assert(config_load_boot_usb());
}

// The wireless target. Only a whole configuration with wl_mode exactly 1 is Wi-Fi; everything else is "none", and then the mode switch never
// leaves USB (mode_switch.c, row 1 of the boot table and step_usb).
static void test_wl_effective(void)
{
    fresh();
    provision(1);
    assert(config_load_wl_effective() == 1);          // whole and on

    fresh();
    provision(0);
    assert(config_load_wl_effective() == 0);          // whole, switched off

    // wl_mode written with no credentials at all (a dial wiped part-way, or a hand-written NVS): no target
    fresh();
    put_u8("wl_mode", 1);
    assert(config_load_wl_effective() == 0);

    // the marker missing (a power cut during a change): the configuration is not whole, so there is no target
    fresh();
    provision(1);
    erase("wl_ver");
    assert(config_load_wl_effective() == 0);

    // a key missing under a present marker
    static const char *const keys[] = {"wl_ssid", "wl_pass", "wl_host", "wl_port", "wl_key"};
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        fresh();
        provision(1);
        erase(keys[i]);
        assert(config_load_wl_effective() == 0);
    }

    // a corrupt wl_mode byte: not 1, so not Wi-Fi
    static const uint8_t bad[] = {2 /* the phone: a byte of 2 over Wi-Fi settings alone is no target (test_ble_config.c has the phone's own settings) */, 3, 7, 128, 255};
    for (unsigned i = 0; i < sizeof bad; i++) {
        fresh();
        provision(1);
        put_u8("wl_mode", bad[i]);
        assert(config_load_wl_effective() == 0);
    }

    // a corrupt marker
    fresh();
    provision(1);
    put_u8("wl_ver", 7);
    assert(config_load_wl_effective() == 0);
}

static void test_forgetting_the_network_takes_the_override(void)
{
    fresh();
    provision(1);
    assert(config_save_stay_wl(true));
    assert(config_net_clear() == CFG_NET_OK);
    assert(!config_load_stay_wl());
    assert(config_load_wl_effective() == 0);
    // a power cut during the clear still leaves the dial without a target (the marker goes first)
    fresh();
    provision(1);
    assert(config_save_stay_wl(true));
    host_nvs_fail_after(-1);
    assert(config_net_clear() == CFG_NET_OK);
    const int total = host_nvs_mutations();
    for (int cut = 1; cut < total; cut++) {
        fresh();
        provision(1);
        assert(config_save_stay_wl(true));
        host_nvs_fail_after(cut);
        (void)config_net_clear();
        host_nvs_fail_after(-1);
        assert(config_load_wl_effective() == 0);
    }
}

static void test_factory_reset_takes_everything(void)
{
    fresh();
    provision(1);
    assert(config_save_stay_wl(true));
    assert(config_save_boot_usb(true));
    assert(config_clear_all());
    assert(!config_load_stay_wl());
    assert(!config_load_boot_usb());
    assert(config_load_wl_effective() == 0);
    assert(!host_nvs_has("stay_wl") && !host_nvs_has("boot_usb") && !host_nvs_has("wl_mode"));
}

// The other net.* changes leave the override alone: it is the user's choice, set in Settings.
static void test_other_changes_keep_the_override(void)
{
    fresh();
    provision(1);
    assert(config_save_stay_wl(true));
    assert(config_net_set_mode(1) == CFG_NET_OK);
    assert(config_net_set_host("10.0.0.5", 18480) == CFG_NET_OK);
    assert(config_net_set_key(KEY) == CFG_NET_OK);
    assert(config_load_stay_wl());
    provision(1);   // a fresh net.set over an existing one
    assert(config_load_stay_wl());
}

int main(void)
{
    test_defaults();
    test_round_trip();
    test_corrupt_bytes_read_false();
    test_boot_usb_written_only_on_change();
    test_wl_effective();
    test_forgetting_the_network_takes_the_override();
    test_factory_reset_takes_everything();
    test_other_changes_keep_the_override();
    puts("ok test_mode_config");
    return 0;
}

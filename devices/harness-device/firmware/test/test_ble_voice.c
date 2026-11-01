// B7: voice in the phone mode. Two rules, against the real code, on the host:
//   1. The codec: a turn over BLE uses ADPCM exactly when the daemon's welcome offered it (voice.adpcm), PCM otherwise; USB keeps PCM (cable_features.h).
//   2. The ring: the phone mode takes the 96 KB PCM ring like Wi-Fi, because a stall of up to 3 s must not lose speech (spec section 5.3; test_audio_stream.py
//      holds the stall cases for that ring), and ONLY a stored mode that is whole gets it. The size expression is cut out of audio_client_habitat.c by
//      test_ble_voice.py and passed in as RING_EXPR, so this runs the real line against the real config_net_info over an in-memory NVS.
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cable_features.h"
#include "config_store.h"

#define BUFFER_BYTES 32768
#define BUFFER_BYTES_WIFI 98304

void host_log_info(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
void host_log_warn(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
extern void host_nvs_reset(void);

static const uint8_t KEY[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

static unsigned ring_for_stored_mode(void)
{
    config_net_info_t net;
    config_net_info(&net);
    unsigned ring_bytes;
    ring_bytes = RING_EXPR;
    return ring_bytes;
}

int main(void)
{
    // 1. the codec: transport x offered x the USB build flag
    for (int usb = 0; usb < 2; usb++) {
        for (int offered = 0; offered < 2; offered++) {
            for (int flag = 0; flag < 2; flag++) {
                const uint32_t features = offered ? CABLE_FEATURE_ADPCM : 0;
                const bool want = offered && (!usb || flag);
                assert(cable_voice_adpcm_chosen(features, usb, flag) == want);
                assert(cable_voice_adpcm_allowed(usb, flag) == (!usb || flag));
            }
        }
    }
    // over BLE (not USB): ADPCM exactly when offered, whatever the USB flag says; with BLE and no offer, PCM
    assert(cable_voice_adpcm_chosen(CABLE_FEATURE_ADPCM | CABLE_FEATURE_BLE, false, false));
    assert(!cable_voice_adpcm_chosen(CABLE_FEATURE_BLE | CABLE_FEATURE_NET, false, false));
    assert(!cable_voice_adpcm_chosen(CABLE_FEATURE_ADPCM, true, false));   // USB keeps PCM

    // 2. the ring
    host_nvs_reset();
    assert(ring_for_stored_mode() == BUFFER_BYTES);                              // nothing stored: USB only, the 32 KB ring
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    assert(ring_for_stored_mode() == BUFFER_BYTES);                              // the key alone is no phone mode
    assert(config_net_set_mode(2) == CFG_NET_OK);
    assert(ring_for_stored_mode() == BUFFER_BYTES_WIFI);                         // the phone mode: 96 KB
    assert(config_net_set_mode(0) == CFG_NET_OK);
    assert(ring_for_stored_mode() == BUFFER_BYTES);                              // switched off: back to 32 KB
    assert(config_net_set("home-net", "correct horse battery", "192.168.1.20", 18480, KEY, 1) == CFG_NET_OK);
    assert(ring_for_stored_mode() == BUFFER_BYTES_WIFI);                         // Wi-Fi: 96 KB as before
    assert(config_net_set_mode(0) == CFG_NET_OK);
    assert(ring_for_stored_mode() == BUFFER_BYTES);
    assert(config_net_clear() == CFG_NET_OK);
    assert(ring_for_stored_mode() == BUFFER_BYTES);
    // a phone dial whose marker is gone (a cut during a change) gets no big ring
    host_nvs_reset();
    assert(config_net_set_ble(KEY) == CFG_NET_OK && config_net_set_mode(2) == CFG_NET_OK);
    assert(ring_for_stored_mode() == BUFFER_BYTES_WIFI);
    assert(config_net_clear() == CFG_NET_OK);
    assert(ring_for_stored_mode() == BUFFER_BYTES);
    puts("ble voice: ADPCM over BLE exactly when offered, PCM on USB, the 96 KB ring only for a whole phone or Wi-Fi mode PASS");
    return 0;
}

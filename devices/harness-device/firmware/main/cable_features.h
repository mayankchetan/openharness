#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct cJSON cJSON;
enum {
    CABLE_FEATURE_FORM = 1u << 0,
    CABLE_FEATURE_SELECTION = 1u << 1,
    CABLE_FEATURE_VISIT = 1u << 2,
    CABLE_FEATURE_DRAFT = 1u << 3,
    CABLE_FEATURE_QUESTIONS = 1u << 4,
    CABLE_FEATURE_AGENTS_REFRESH = 1u << 5,
    // The daemon will carry this device's preferences to the desktop app and send back changes. A
    // device that does not see it keeps its own settings screens; nothing here is load-bearing for a
    // session, so an older daemon simply never asks.
    CABLE_FEATURE_SETTINGS = 1u << 6,
    // The host speaks the debug.* commands (docs/DEBUG_COMMANDS.md). Informational: the device answers
    // them with or without it, because a host script that only wants a screenshot has no reason to
    // play daemon and send a welcome first.
    CABLE_FEATURE_DEBUG = 1u << 7,
    // The daemon decodes ADPCM voice blocks (frame type 0x07, adpcm_ima.h). Offered only to a dial whose hello said
    // voiceCodec "adpcm-v1"; the dial still chooses per turn, and on USB it keeps sending PCM unless built otherwise.
    CABLE_FEATURE_ADPCM = 1u << 8,
    // The daemon keeps this dial's Wi-Fi settings and speaks net.set / get / clear / mode / key / host (docs/specs/cable-wifi-link.md section 3).
    // Offered only over the USB cable, to a dial whose hello said net "v1". The dial answers none of them without it.
    CABLE_FEATURE_NET = 1u << 9,
    // The daemon keeps this dial's phone-mode key and takes the key-only net.set and net.mode 2 (docs/specs/cable-ble-link.md). Offered only over the
    // USB cable, to a dial whose hello said ble "v1" (an image built with the BLE link). The dial refuses both forms without it.
    CABLE_FEATURE_BLE = 1u << 10,
};

// Does this image carry the BLE link? DEVICE_BLE_LINK is defined by main/CMakeLists.txt exactly when NimBLE is in the build (CONFIG_BT_NIMBLE_ENABLED).
// The default image has none: its hello does not say "ble", and it refuses the phone-mode forms of net.set and net.mode whatever the daemon offers.
static inline bool cable_ble_link_built(void)
{
#ifdef DEVICE_BLE_LINK
    return true;
#else
    return false;
#endif
}

// Which voice codec a turn uses (task W4; docs/specs/cable-wifi-link.md section 2.5). Pure and IDF-free so the host test pins the
// rule. `transport_is_usb`: the cable is the USB link (the only link today; Wi-Fi comes later). `usb_flag`: the build flag
// DEVICE_VOICE_ADPCM_USB, which lets gate G1 run ADPCM over USB before any Wi-Fi exists.
//   allowed: this dial may use ADPCM on this transport. It is what the hello advertises (voiceCodec "adpcm-v1"), so a
//            daemon never offers voice.adpcm to a dial that would ignore it.
//   chosen : the daemon's welcome has voice.adpcm AND the dial may use it. voice.begin.codec = "adpcm" only then.
static inline bool cable_voice_adpcm_allowed(bool transport_is_usb, bool usb_flag) { return !transport_is_usb || usb_flag; }
static inline bool cable_voice_adpcm_chosen(uint32_t features, bool transport_is_usb, bool usb_flag)
{
    return (features & CABLE_FEATURE_ADPCM) && cable_voice_adpcm_allowed(transport_is_usb, usb_flag);
}

// Optional welcome field. Older daemons omit it and retain the core voice,
// pane, tab, scroll, machine and model controls. Unknown names are ignored.
uint32_t cable_features_parse(const cJSON *welcome);

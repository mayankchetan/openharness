#include "cable_features.h"
#include "cJSON.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned allocations;
static void *allocate(size_t bytes) { allocations++; return malloc(bytes); }
static void check(const char *json, uint32_t expected)
{
    cJSON *welcome = cJSON_Parse(json);
    assert(welcome);
    unsigned before = allocations;
    assert(cable_features_parse(welcome) == expected);
    assert(allocations == before);
    cJSON_Delete(welcome);
}
int main(void)
{
    cJSON_Hooks hooks = {.malloc_fn=allocate, .free_fn=free};
    cJSON_InitHooks(&hooks);
    assert(!cable_features_parse(NULL));
    check("{}", 0);
    check("{\"features\":null}", 0);
    check("{\"features\":true}", 0);
    check("{\"features\":\"form\"}", 0);
    check("{\"features\":[]}", 0);
    check("{\"features\":[\"form\",\"form\",\"future\"]}", CABLE_FEATURE_FORM);
    check("{\"features\":[\"Form\",\"FORM\",\"form.2\"]}", 0);
    check("{\"features\":[\"form\",null]}", 0);
    check("{\"features\":[\"form\",{}]}", 0);
    check("{\"features\":[\"form\",[\"visit\"]]}", 0);
    check("{\"features\":[\"agents.refresh\"]}", CABLE_FEATURE_AGENTS_REFRESH);
    // ADPCM voice (frame type 0x07): bit 8, named "voice.adpcm". The bit is wire-visible to the daemon's feature list,
    // so its value is pinned here, not just referenced.
    assert(CABLE_FEATURE_ADPCM == (1u << 8));
    check("{\"features\":[\"voice.adpcm\"]}", CABLE_FEATURE_ADPCM);
    check("{\"features\":[\"voice.adpcm\",\"voice.adpcm\"]}", CABLE_FEATURE_ADPCM);
    check("{\"features\":[\"Voice.adpcm\",\"voice.ADPCM\",\"voice.adpcm.2\",\"adpcm\"]}", 0);
    check("{\"features\":[\"voice.draft\",\"voice.adpcm\",\"debug\"]}", CABLE_FEATURE_DRAFT | CABLE_FEATURE_ADPCM | CABLE_FEATURE_DEBUG);
    check("{\"features\":[\"voice.adpcm\",{}]}", 0);
    check("{\"features\":[\"debug\"]}", CABLE_FEATURE_DEBUG);
    check("{\"features\":[\"debug\",\"form\"]}", CABLE_FEATURE_DEBUG | CABLE_FEATURE_FORM);
    check("{\"features\":[\"Debug\",\"debug.shot\"]}", 0);
    // Wi-Fi settings (net.*): bit 9, named "net". Wire-visible to the daemon, so its value is pinned here too.
    assert(CABLE_FEATURE_NET == (1u << 9));
    check("{\"features\":[\"net\"]}", CABLE_FEATURE_NET);
    check("{\"features\":[\"Net\",\"net.set\",\"network\"]}", 0);
    check("{\"features\":[\"voice.adpcm\",\"net\",\"debug\"]}", CABLE_FEATURE_ADPCM | CABLE_FEATURE_NET | CABLE_FEATURE_DEBUG);
    // Phone mode (B6): bit 10, named "ble". Wire-visible to the daemon, so its value is pinned here too.
    assert(CABLE_FEATURE_BLE == (1u << 10));
    check("{\"features\":[\"ble\"]}", CABLE_FEATURE_BLE);
    check("{\"features\":[\"net\",\"ble\"]}", CABLE_FEATURE_NET | CABLE_FEATURE_BLE);
    check("{\"features\":[\"Ble\",\"ble.set\",\"bluetooth\"]}", 0);
    // this host test is built without DEVICE_BLE_LINK: the image has no BLE link, and says so
    assert(!cable_ble_link_built());
    static const char *names[] = {"form", "selection", "visit", "voice.draft", "question.review", "agents.refresh"};
    for (unsigned mask=0; mask<64; mask++) {
        cJSON *root=cJSON_CreateObject(), *array=cJSON_AddArrayToObject(root,"features");
        assert(root && array);
        for (unsigned i=0;i<6;i++) if (mask & (1u<<i))
            assert(cJSON_AddItemToArray(array,cJSON_CreateString(names[i])));
        assert(cable_features_parse(root)==mask);
        cJSON_Delete(root);
    }
    cJSON *root=cJSON_CreateObject(), *array=cJSON_AddArrayToObject(root,"features");
    assert(root && array);
    for (int i=0;i<16;i++) assert(cJSON_AddItemToArray(array,cJSON_CreateString("form")));
    assert(cable_features_parse(root)==CABLE_FEATURE_FORM);
    assert(cJSON_AddItemToArray(array,cJSON_CreateString("form")));
    assert(cable_features_parse(root)==0);
    cJSON_Delete(root);
    // The ADPCM codec choice (W4). The dial offers ADPCM in its hello only when it would use it, and uses it only when the
    // daemon welcomed voice.adpcm AND (the transport is not USB OR the USB build flag is on).
    for (unsigned usb = 0; usb < 2; usb++)
        for (unsigned flag = 0; flag < 2; flag++) {
            const bool allowed = !usb || flag;
            assert(cable_voice_adpcm_allowed(usb, flag) == allowed);
            assert(cable_voice_adpcm_chosen(CABLE_FEATURE_ADPCM, usb, flag) == allowed);
            assert(!cable_voice_adpcm_chosen(0, usb, flag));
            assert(!cable_voice_adpcm_chosen(~CABLE_FEATURE_ADPCM, usb, flag));
            assert(cable_voice_adpcm_chosen(~0u, usb, flag) == allowed);
        }
    assert(!cable_voice_adpcm_chosen(CABLE_FEATURE_ADPCM, true, false));   // USB, flag off: PCM exactly as before
    assert(cable_voice_adpcm_chosen(CABLE_FEATURE_ADPCM, true, true));     // USB, flag on: gate G1
    assert(cable_voice_adpcm_chosen(CABLE_FEATURE_ADPCM, false, false));   // Wi-Fi: ADPCM without the flag
    puts("host features: PASS (legacy default, 64 combinations, voice.adpcm, codec choice rule, unknown/type/size bounds, no allocations)");
}

#include "cable_debug.h"

#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "cable_link.h"
#include "debug_cmd.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ui/display.h"
#include "ui/touch.h"

static const char *TAG = "cable_debug";

// How long the renderer gets to answer a capture. A frame takes milliseconds; this only has to outlast the
// slowest one plus a notification, and be short enough that a wedged renderer is reported rather than
// waited on.
#define CAPTURE_WAIT_MS 2000

// One chunk's frame body. Static because only the reader task sends a screenshot, and the reader's 6 KiB
// stack already carries the JSON parse that got us here.
static EXT_RAM_BSS_ATTR uint8_t s_chunk[DBG_SHOT_HEADER + DBG_SHOT_CHUNK];   // PSRAM: copied by cable_link_send

// The gesture in flight, for the reply that comes later from the tick. Reader task only: it is written
// right after a successful arm, on the same task whose tick reads it, so the two cannot interleave.
static dbg_cmd_t s_touch_cmd;
static uint16_t  s_touch_id;

// Send a reply built by debug_cmd_json.c, which hands over ownership of the text.
static bool send_text(char *text)
{
    if (!text) return false;
    const bool ok = cable_link_send(CABLE_TYPE_JSON, (const uint8_t *)text, strlen(text));
    cJSON_free(text);
    return ok;
}

// Every refusal is logged here and told to the host, so a script never has to guess why nothing came back.
static void refuse(dbg_cmd_t cmd, uint16_t id, dbg_err_t err)
{
    ESP_LOGW(TAG, "%s id=%u refused: %s", dbg_cmd_name(cmd), (unsigned)id, dbg_err_name(err));
    send_text(dbg_reply_error(cmd, id, err));
}

// ── screenshot ──────────────────────────────────────────────────────────────────────────────────────

static void shot(uint16_t id)
{
    // The picture is 424 KiB, in PSRAM, for the length of the transfer and not a byte longer. Ask first
    // whether the heap can spare it: a refusal costs the host one retry, a squeezed heap costs the dial.
    if (!dbg_shot_memory_ok(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                            heap_caps_get_free_size(MALLOC_CAP_SPIRAM))) {
        refuse(DBG_CMD_SHOT, id, DBG_E_MEMORY);
        return;
    }
    uint8_t *pixels = heap_caps_malloc(DBG_SHOT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        refuse(DBG_CMD_SHOT, id, DBG_E_MEMORY);
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    const dbg_err_t err = display_capture((uint16_t *)pixels, CAPTURE_WAIT_MS);
    if (err) {
        heap_caps_free(pixels);
        refuse(DBG_CMD_SHOT, id, err);
        return;
    }
    const int64_t t1 = esp_timer_get_time();

    const uint32_t chunks = dbg_shot_chunks(DBG_SHOT_BYTES, DBG_SHOT_CHUNK);
    uint32_t crc = 0, sent = 0;
    bool ok = send_text(dbg_reply_shot_begin(id));
    for (; ok && sent < chunks; sent++) {
        const size_t n = dbg_shot_chunk_encode(id, sent, pixels, DBG_SHOT_BYTES, DBG_SHOT_CHUNK, s_chunk,
                                               sizeof s_chunk);
        ok = n > DBG_SHOT_HEADER && cable_link_send(CABLE_TYPE_SHOT, s_chunk, n);
        if (ok) crc = dbg_crc32(crc, s_chunk + DBG_SHOT_HEADER, n - DBG_SHOT_HEADER);
    }
    heap_caps_free(pixels);
    if (!ok) {
        // The host stopped draining the port. Nothing it could still receive is worth the wait; the end
        // frame is deliberately not sent, so a half picture can never look finished.
        ESP_LOGW(TAG, "shot id=%u abandoned at chunk %u of %u: host not reading", (unsigned)id,
                 (unsigned)sent, (unsigned)chunks);
        return;
    }
    send_text(dbg_reply_shot_end(id, chunks, crc));
    ESP_LOGI(TAG, "shot id=%u: %u B in %u chunks, capture %lu ms, send %lu ms, crc32 %08lx", (unsigned)id,
             (unsigned)DBG_SHOT_BYTES, (unsigned)chunks, (unsigned long)((t1 - t0) / 1000),
             (unsigned long)((esp_timer_get_time() - t1) / 1000), (unsigned long)crc);
}

// ── touch ───────────────────────────────────────────────────────────────────────────────────────────

static void touch(const dbg_request_t *r)
{
    // The touch task is where an injected contact is played, and it only runs its loop with a controller.
    touch_stats_t stats;
    touch_stats(&stats);
    if (!stats.controller_ok) {
        refuse(r->cmd, r->id, DBG_E_TOUCH);
        return;
    }
    if (!dbg_inject_arm(&r->gesture, esp_timer_get_time())) {
        refuse(r->cmd, r->id, DBG_E_BUSY);
        return;
    }
    s_touch_cmd = r->cmd;
    s_touch_id = r->id;
    touch_inject_wake();
}

void cable_debug_tick(void)
{
    const int outcome = dbg_inject_finish(esp_timer_get_time());
    if (outcome > 0) send_text(dbg_reply_done(s_touch_cmd, s_touch_id));
    else if (outcome < 0) refuse(s_touch_cmd, s_touch_id, DBG_E_TOUCH);
}

bool cable_debug_handle(const char *t, const cJSON *p)
{
    dbg_request_t request;
    const dbg_err_t err = dbg_request_parse(t, p, &request);
    if (err == DBG_NOT_DEBUG) return false;
    if (err) refuse(request.cmd, request.id, err);
    else if (request.cmd == DBG_CMD_SHOT) shot(request.id);
    else touch(&request);
    return true;
}

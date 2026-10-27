// The JSON half of the debug commands: reading a request and building a reply. Separate from debug_cmd.c
// only so that file needs no cJSON on a host that has no ESP-IDF; test_debug_cmd_json.c runs this one
// against the SDK's own cJSON, like test_cable_features.c.
#include "debug_cmd.h"

#include <math.h>
#include <string.h>

#include "cJSON.h"

// Absent is NAN, which every maker above refuses as a shape error: a missing field and a wrong-typed one
// are the same mistake to the host.
static double number_of(const cJSON *p, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(p, key);
    return cJSON_IsNumber(item) ? item->valuedouble : NAN;
}

// `id` is optional, but if it is there it must be a whole number that fits the chunk header's u16.
static dbg_err_t id_of(const cJSON *p, uint16_t *out)
{
    *out = 0;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(p, "id");
    if (!item) return DBG_OK;
    if (!cJSON_IsNumber(item) || item->valuedouble != floor(item->valuedouble)) return DBG_E_SHAPE;
    if (item->valuedouble < 0 || item->valuedouble > UINT16_MAX) return DBG_E_RANGE;
    *out = (uint16_t)item->valuedouble;
    return DBG_OK;
}

dbg_err_t dbg_request_parse(const char *t, const cJSON *p, dbg_request_t *out)
{
    *out = (dbg_request_t){0};
    if (!t || strncmp(t, "debug.", 6)) return DBG_NOT_DEBUG;
    static const struct { const char *name; dbg_cmd_t cmd; } names[] = {
        {"debug.shot", DBG_CMD_SHOT}, {"debug.tap", DBG_CMD_TAP},
        {"debug.press", DBG_CMD_PRESS}, {"debug.swipe", DBG_CMD_SWIPE},
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(t, names[i].name)) out->cmd = names[i].cmd;
    // A debug.* name this build has no case for is not ours to refuse: it may be a newer host's, and the
    // caller counts it with every other unknown message.
    if (out->cmd == DBG_CMD_NONE) return DBG_NOT_DEBUG;
    if (!cJSON_IsObject(p)) return DBG_E_SHAPE;
    dbg_err_t err = id_of(p, &out->id);
    if (err) return err;
    switch (out->cmd) {
    case DBG_CMD_TAP:
        return dbg_tap_make(number_of(p, "x"), number_of(p, "y"), &out->gesture);
    case DBG_CMD_PRESS:
        return dbg_press_make(number_of(p, "x"), number_of(p, "y"), number_of(p, "ms"), &out->gesture);
    case DBG_CMD_SWIPE:
        return dbg_swipe_make(number_of(p, "x1"), number_of(p, "y1"), number_of(p, "x2"), number_of(p, "y2"),
                              number_of(p, "ms"), &out->gesture);
    default:
        return DBG_OK;
    }
}

const char *dbg_cmd_name(dbg_cmd_t cmd)
{
    switch (cmd) {
    case DBG_CMD_SHOT:  return "shot";
    case DBG_CMD_TAP:   return "tap";
    case DBG_CMD_PRESS: return "press";
    case DBG_CMD_SWIPE: return "swipe";
    default:            return "";
    }
}

// Take ownership of `root`, print it, free the tree — the shape of cable_client.c's send_json, ending in
// the text instead of the write. NULL on any allocation failure, never a half-built reply.
static char *print(cJSON *root, bool ok)
{
    if (!root) return NULL;
    char *text = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return text;
}

static cJSON *reply(const char *t, uint16_t id, bool *ok)
{
    cJSON *root = cJSON_CreateObject();
    *ok = root && cJSON_AddStringToObject(root, "t", t) && cJSON_AddNumberToObject(root, "id", id);
    return root;
}

char *dbg_reply_error(dbg_cmd_t cmd, uint16_t id, dbg_err_t err)
{
    bool ok;
    cJSON *root = reply("debug.error", id, &ok);
    ok = ok && cJSON_AddStringToObject(root, "cmd", dbg_cmd_name(cmd)) &&
         cJSON_AddStringToObject(root, "reason", dbg_err_name(err));
    return print(root, ok);
}

char *dbg_reply_done(dbg_cmd_t cmd, uint16_t id)
{
    bool ok;
    cJSON *root = reply("debug.done", id, &ok);
    ok = ok && cJSON_AddStringToObject(root, "cmd", dbg_cmd_name(cmd));
    return print(root, ok);
}

char *dbg_reply_shot_begin(uint16_t id)
{
    bool ok;
    cJSON *root = reply("debug.shot.begin", id, &ok);
    ok = ok && cJSON_AddNumberToObject(root, "w", HT_WIDTH) && cJSON_AddNumberToObject(root, "h", HT_HEIGHT) &&
         cJSON_AddStringToObject(root, "fmt", DBG_SHOT_FMT) &&
         cJSON_AddNumberToObject(root, "bytes", DBG_SHOT_BYTES) &&
         cJSON_AddNumberToObject(root, "chunk", DBG_SHOT_CHUNK) &&
         cJSON_AddNumberToObject(root, "chunks", dbg_shot_chunks(DBG_SHOT_BYTES, DBG_SHOT_CHUNK));
    return print(root, ok);
}

char *dbg_reply_shot_end(uint16_t id, uint32_t chunks, uint32_t crc)
{
    bool ok;
    cJSON *root = reply("debug.shot.end", id, &ok);
    ok = ok && cJSON_AddNumberToObject(root, "chunks", chunks) && cJSON_AddNumberToObject(root, "crc32", crc);
    return print(root, ok);
}

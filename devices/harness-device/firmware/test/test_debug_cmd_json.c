// Host test for debug_cmd_json.c against the cJSON the board links: reading a request off the wire and
// the exact bytes of every reply. Runs only where the SDK is, like test_cable_features.c.
#include "debug_cmd.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static long live;             // allocations not yet freed
static long fail_after = -1;  // fail the Nth allocation from now; -1 never
static void *allocate(size_t n)
{
    if (fail_after >= 0 && fail_after-- == 0) return NULL;
    void *p = malloc(n);
    if (p) live++;
    return p;
}
static void release(void *p)
{
    if (p) live--;
    free(p);
}

// Parse `json` as one message, the way cable_client.c does: `t` from the object, body = the object itself.
static dbg_err_t parse(const char *json, dbg_request_t *out)
{
    cJSON *root = cJSON_Parse(json);
    assert(root);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "t");
    dbg_err_t err = dbg_request_parse(cJSON_IsString(t) ? t->valuestring : NULL, root, out);
    cJSON_Delete(root);
    return err;
}

static void test_not_ours(void)
{
    dbg_request_t r;
    assert(parse("{\"t\":\"ping\"}", &r) == DBG_NOT_DEBUG && r.cmd == DBG_CMD_NONE);
    assert(parse("{\"t\":\"debugger\"}", &r) == DBG_NOT_DEBUG);
    assert(parse("{\"t\":\"debug.future\"}", &r) == DBG_NOT_DEBUG);   // a newer host's, left to the unknown count
    assert(parse("{\"t\":\"debug.\"}", &r) == DBG_NOT_DEBUG);
    assert(parse("{\"t\":\"Debug.shot\"}", &r) == DBG_NOT_DEBUG);     // case matters, as everywhere else
    assert(parse("{\"t\":\"debug.shot.begin\"}", &r) == DBG_NOT_DEBUG);   // our own replies are not requests
    assert(parse("{\"x\":1}", &r) == DBG_NOT_DEBUG);
    assert(dbg_request_parse(NULL, NULL, &r) == DBG_NOT_DEBUG);
}

static void test_shot(void)
{
    dbg_request_t r;
    assert(parse("{\"t\":\"debug.shot\"}", &r) == DBG_OK && r.cmd == DBG_CMD_SHOT && r.id == 0);
    assert(parse("{\"t\":\"debug.shot\",\"id\":7}", &r) == DBG_OK && r.id == 7);
    assert(parse("{\"t\":\"debug.shot\",\"id\":65535}", &r) == DBG_OK && r.id == 65535);
    assert(parse("{\"t\":\"debug.shot\",\"id\":0}", &r) == DBG_OK && r.id == 0);
    assert(parse("{\"t\":\"debug.shot\",\"id\":7,\"extra\":[1,2],\"x\":\"no\"}", &r) == DBG_OK && r.id == 7);   // lenient
    // A bad id is refused, and the refusal still knows the command.
    assert(parse("{\"t\":\"debug.shot\",\"id\":65536}", &r) == DBG_E_RANGE && r.cmd == DBG_CMD_SHOT);
    assert(parse("{\"t\":\"debug.shot\",\"id\":-1}", &r) == DBG_E_RANGE);
    assert(parse("{\"t\":\"debug.shot\",\"id\":1.5}", &r) == DBG_E_SHAPE);
    assert(parse("{\"t\":\"debug.shot\",\"id\":\"7\"}", &r) == DBG_E_SHAPE);
    assert(parse("{\"t\":\"debug.shot\",\"id\":null}", &r) == DBG_E_SHAPE);
    assert(parse("{\"t\":\"debug.shot\",\"id\":true}", &r) == DBG_E_SHAPE);
    // `p` form: the body under "p", as handle_message accepts.
    cJSON *root = cJSON_Parse("{\"t\":\"debug.shot\",\"p\":{\"id\":9}}");
    assert(root && dbg_request_parse("debug.shot", cJSON_GetObjectItemCaseSensitive(root, "p"), &r) == DBG_OK && r.id == 9);
    cJSON_Delete(root);
    // A body that is not an object at all.
    root = cJSON_Parse("{\"t\":\"debug.shot\",\"p\":[1]}");
    assert(root && dbg_request_parse("debug.shot", cJSON_GetObjectItemCaseSensitive(root, "p"), &r) == DBG_E_SHAPE &&
           r.cmd == DBG_CMD_SHOT);
    cJSON_Delete(root);
    assert(dbg_request_parse("debug.shot", NULL, &r) == DBG_E_SHAPE && r.cmd == DBG_CMD_SHOT);
}

static void test_touch(void)
{
    dbg_request_t r;
    assert(parse("{\"t\":\"debug.tap\",\"id\":3,\"x\":233,\"y\":120}", &r) == DBG_OK && r.cmd == DBG_CMD_TAP && r.id == 3 &&
           r.gesture.kind == DBG_TAP && r.gesture.x1 == 233 && r.gesture.y1 == 120);
    assert(parse("{\"t\":\"debug.tap\",\"x\":233.0,\"y\":120}", &r) == DBG_OK);   // 233.0 is a whole number
    assert(parse("{\"t\":\"debug.tap\",\"x\":233,\"y\":120,\"ms\":900}", &r) == DBG_OK && r.gesture.ms == DBG_TAP_MS);
    assert(parse("{\"t\":\"debug.tap\",\"x\":233}", &r) == DBG_E_SHAPE && r.cmd == DBG_CMD_TAP);
    assert(parse("{\"t\":\"debug.tap\",\"y\":233}", &r) == DBG_E_SHAPE);
    assert(parse("{\"t\":\"debug.tap\"}", &r) == DBG_E_SHAPE);
    assert(parse("{\"t\":\"debug.tap\",\"x\":\"233\",\"y\":233}", &r) == DBG_E_SHAPE);
    assert(parse("{\"t\":\"debug.tap\",\"x\":null,\"y\":233}", &r) == DBG_E_SHAPE);
    assert(parse("{\"t\":\"debug.tap\",\"x\":[233],\"y\":233}", &r) == DBG_E_SHAPE);
    assert(parse("{\"t\":\"debug.tap\",\"x\":233.5,\"y\":233}", &r) == DBG_E_SHAPE);
    assert(parse("{\"t\":\"debug.tap\",\"x\":0,\"y\":0}", &r) == DBG_E_RANGE);
    assert(parse("{\"t\":\"debug.tap\",\"x\":466,\"y\":233}", &r) == DBG_E_RANGE);
    assert(parse("{\"t\":\"debug.tap\",\"x\":-3,\"y\":233}", &r) == DBG_E_RANGE);
    assert(parse("{\"t\":\"debug.tap\",\"x\":1e999,\"y\":233}", &r) != DBG_OK);     // cJSON reads this as inf
    assert(parse("{\"t\":\"debug.tap\",\"id\":70000,\"x\":233,\"y\":233}", &r) == DBG_E_RANGE && r.cmd == DBG_CMD_TAP);

    assert(parse("{\"t\":\"debug.press\",\"x\":233,\"y\":233,\"ms\":700}", &r) == DBG_OK && r.cmd == DBG_CMD_PRESS &&
           r.gesture.kind == DBG_PRESS && r.gesture.ms == 700);
    assert(parse("{\"t\":\"debug.press\",\"x\":233,\"y\":233}", &r) == DBG_E_SHAPE);   // a press needs its length
    assert(parse("{\"t\":\"debug.press\",\"x\":233,\"y\":233,\"ms\":10}", &r) == DBG_E_RANGE);
    assert(parse("{\"t\":\"debug.press\",\"x\":233,\"y\":233,\"ms\":6000}", &r) == DBG_E_RANGE);

    assert(parse("{\"t\":\"debug.swipe\",\"id\":4,\"x1\":100,\"y1\":233,\"x2\":300,\"y2\":233,\"ms\":200}", &r) == DBG_OK &&
           r.cmd == DBG_CMD_SWIPE && r.id == 4 && r.gesture.kind == DBG_SWIPE && r.gesture.x1 == 100 &&
           r.gesture.x2 == 300 && r.gesture.ms == 200);
    assert(parse("{\"t\":\"debug.swipe\",\"x1\":100,\"y1\":233,\"x2\":300,\"y2\":233}", &r) == DBG_E_SHAPE);     // no ms
    assert(parse("{\"t\":\"debug.swipe\",\"x1\":100,\"y1\":233,\"x2\":300,\"ms\":200}", &r) == DBG_E_SHAPE);     // no y2
    assert(parse("{\"t\":\"debug.swipe\",\"x1\":0,\"y1\":0,\"x2\":300,\"y2\":233,\"ms\":200}", &r) == DBG_E_RANGE);
    assert(parse("{\"t\":\"debug.swipe\",\"x1\":100,\"y1\":233,\"x2\":300,\"y2\":233,\"ms\":5}", &r) == DBG_E_RANGE);
    assert(parse("{\"t\":\"debug.swipe\",\"x1\":100,\"y1\":233,\"x2\":300,\"y2\":233,\"ms\":4000}", &r) == DBG_E_RANGE);
}

// The reply, parsed back, field by field: what a host script will do.
static cJSON *reply_of(char *text)
{
    assert(text);
    cJSON *root = cJSON_Parse(text);
    assert(root);
    cJSON_free(text);
    return root;
}
static const char *str(const cJSON *o, const char *k)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    assert(cJSON_IsString(i));
    return i->valuestring;
}
static double num(const cJSON *o, const char *k)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    assert(cJSON_IsNumber(i));
    return i->valuedouble;
}

static void test_replies(void)
{
    // The exact bytes of the two that carry the picture's description, so a host written against
    // docs/DEBUG_COMMANDS.md and this test agree on more than field names.
    char *text = dbg_reply_shot_begin(7);
    assert(text && !strcmp(text, "{\"t\":\"debug.shot.begin\",\"id\":7,\"w\":466,\"h\":466,\"fmt\":\"rgb565be\","
                                 "\"bytes\":434312,\"chunk\":1024,\"chunks\":425}"));
    cJSON_free(text);
    text = dbg_reply_shot_end(7, 425, 0xcbf43926u);
    assert(text && !strcmp(text, "{\"t\":\"debug.shot.end\",\"id\":7,\"chunks\":425,\"crc32\":3421780262}"));
    cJSON_free(text);
    // The largest crc still prints as a whole number a JSON reader can take into a u32.
    text = dbg_reply_shot_end(65535, 425, 0xffffffffu);
    assert(text && strstr(text, "\"crc32\":4294967295}") && strstr(text, "\"id\":65535"));
    cJSON_free(text);
    text = dbg_reply_shot_end(0, 0, 0);
    assert(text && strstr(text, "\"crc32\":0}"));
    cJSON_free(text);

    cJSON *r = reply_of(dbg_reply_error(DBG_CMD_SHOT, 12, DBG_E_MEMORY));
    assert(!strcmp(str(r, "t"), "debug.error") && num(r, "id") == 12 && !strcmp(str(r, "cmd"), "shot") &&
           !strcmp(str(r, "reason"), "memory"));
    cJSON_Delete(r);
    r = reply_of(dbg_reply_error(DBG_CMD_SWIPE, 0, DBG_E_RANGE));
    assert(!strcmp(str(r, "cmd"), "swipe") && !strcmp(str(r, "reason"), "range") && num(r, "id") == 0);
    cJSON_Delete(r);
    r = reply_of(dbg_reply_error(DBG_CMD_NONE, 0, DBG_E_SHAPE));   // an error on a message with no readable command
    assert(!strcmp(str(r, "cmd"), "") && !strcmp(str(r, "reason"), "shape"));
    cJSON_Delete(r);
    r = reply_of(dbg_reply_done(DBG_CMD_TAP, 3));
    assert(!strcmp(str(r, "t"), "debug.done") && num(r, "id") == 3 && !strcmp(str(r, "cmd"), "tap"));
    cJSON_Delete(r);
    r = reply_of(dbg_reply_done(DBG_CMD_PRESS, 65535));
    assert(!strcmp(str(r, "cmd"), "press") && num(r, "id") == 65535);
    cJSON_Delete(r);

    // Each reply is a valid cable message: a JSON object small enough for one frame.
    for (int e = DBG_E_SHAPE; e <= DBG_E_IO; e++) {
        text = dbg_reply_error(DBG_CMD_SWIPE, 65535, (dbg_err_t)e);
        assert(text && strlen(text) < 200);
        cJSON_free(text);
    }
}

// An allocation that fails mid-reply gives back NULL, never a half-built message, and leaks nothing.
static void test_allocation_failure(void)
{
    for (long n = 0; n < 40; n++) {
        char *a, *b, *c, *d;
        long before = live;
        fail_after = n;
        a = dbg_reply_shot_begin(1);
        fail_after = n;
        b = dbg_reply_shot_end(1, 425, 99);
        fail_after = n;
        c = dbg_reply_error(DBG_CMD_TAP, 1, DBG_E_BUSY);
        fail_after = n;
        d = dbg_reply_done(DBG_CMD_TAP, 1);
        fail_after = -1;
        // Whatever came back is complete: it parses, and ends in a closing brace.
        char *all[] = {a, b, c, d};
        for (int i = 0; i < 4; i++)
            if (all[i]) {
                cJSON *p = cJSON_Parse(all[i]);
                assert(p && cJSON_IsObject(p));
                cJSON_Delete(p);
                cJSON_free(all[i]);
            }
        assert(live == before);
    }
}

int main(void)
{
    cJSON_Hooks hooks = {.malloc_fn = allocate, .free_fn = release};
    cJSON_InitHooks(&hooks);
    test_not_ours();
    test_shot();
    test_touch();
    test_replies();
    test_allocation_failure();
    assert(live == 0);
    puts("debug commands json: PASS (request shapes, id and coordinate validation, exact reply bytes, allocation failure leaves nothing)");
}

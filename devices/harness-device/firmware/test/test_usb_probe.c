// usb_probe_core.c (task W15): the part of the USB probe that needs no driver. A wireless dial that sees a host on its USB port says hello with
// probe:1 every 2 s and looks for ONE thing in what comes back: a JSON frame whose top-level "t" is "welcome". The rest of this file is the
// ways that can go wrong, because a probe that misses a welcome is a dial that never comes back from Wi-Fi to USB, and a probe that takes
// anything else for a welcome is a dial that reboots into USB when there is no daemon (the "PC without the daemon" case, MS H3).
//
//   - hello: the exact frame; small; right product and protocol; carries probe:1
//   - welcome detection: the daemon's probe welcome, an old daemon's full welcome, whitespace, key order; and not a ping, a nested "t", a quoted
//     "t", a log line that spells a welcome, a truncated or doubled one
//   - the frame cap: 1,024 bytes of frame, header and CRC included. A frame of exactly that is read, one byte over is noise that the decoder
//     steps past, and the next welcome is still found
//   - split reads, bootloader chatter in front, a roster burst from an old daemon behind
//   - the cadence: first hello at once, then every 2 s, one hello (not a burst) after a stall; a new plug-in forgets old welcomes
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cable_frame.h"
#include "usb_probe_core.h"

static uint8_t g_dec[UP_FRAME_CAP];
static up_core_t g_p;

static size_t frame(uint8_t type, const char *payload, size_t len, uint8_t *out, size_t cap)
{
    int n = cable_frame_encode(type, (const uint8_t *)payload, len, out, cap);
    assert(n > 0);
    return (size_t)n;
}
static size_t json_frame(const char *json, uint8_t *out, size_t cap) { return frame(CABLE_TYPE_JSON, json, strlen(json), out, cap); }

static void fresh(void)
{
    memset(&g_p, 0, sizeof g_p);
    assert(up_core_init(&g_p, g_dec, sizeof g_dec));
    up_core_start(&g_p, 0);
}
static void feed_json(const char *json, uint64_t now)
{
    uint8_t f[CABLE_MAX_FRAME];
    up_core_feed(&g_p, f, json_frame(json, f, sizeof f), now);
}

static char s_seen[256];
static int s_count;
static void hello_cb(uint8_t v, uint8_t type, const uint8_t *p, size_t len, void *ctx)
{
    (void)ctx;
    assert(v == CABLE_FRAME_VERSION);
    assert(type == CABLE_TYPE_JSON);
    assert(len < sizeof s_seen);
    memcpy(s_seen, p, len);
    s_seen[len] = 0;
    s_count++;
}

static void test_hello_frame(void)
{
    uint8_t out[128];
    const size_t n = up_core_hello(out, sizeof out, 4, "harness");
    assert(n > 0 && n < 80);
    cable_decoder_t d;
    cable_decoder_init(&d);
    s_count = 0;
    s_seen[0] = 0;
    cable_decoder_feed(&d, out, n, hello_cb, NULL);
    assert(s_count == 1);
    assert(strcmp(s_seen, "{\"t\":\"hello\",\"product\":\"harness\",\"proto\":4,\"probe\":1}") == 0);
    assert(d.corrupt_frames == 0 && d.discarded_bytes == 0);
    // nothing else is in it: no mac, no settings, no pet list, no capability
    assert(!strstr(s_seen, "mac") && !strstr(s_seen, "settings") && !strstr(s_seen, "pets") && !strstr(s_seen, "net"));
    // refusals write nothing and answer 0
    assert(up_core_hello(out, 10, 4, "harness") == 0);
    assert(up_core_hello(NULL, 128, 4, "harness") == 0);
    assert(up_core_hello(out, sizeof out, 4, NULL) == 0);
    assert(up_core_hello(out, sizeof out, 4, "has\"quote") == 0);   // the product is a plain word; anything that needs escaping is refused
    assert(up_core_hello(out, sizeof out, -1, "harness") == 0);
}

static void test_welcome_detection(void)
{
    static const struct { const char *json; int want; const char *why; } t[] = {
        {"{\"t\":\"welcome\",\"proto\":4,\"app\":\"harness\",\"probe\":1}", 1, "the daemon's probe welcome"},
        {"{\"t\":\"welcome\"}", 1, "the least"},
        {"{ \"t\" : \"welcome\" , \"x\" : 1 }", 1, "whitespace"},
        {"{\"proto\":4,\"app\":\"harness\",\"t\":\"welcome\"}", 1, "t last"},
        {"{\"machine\":{\"id\":\"m\",\"name\":\"Mac\"},\"features\":[\"voice.draft\",\"settings\"],\"t\":\"welcome\",\"n\":[1,2,{\"a\":[]}]}", 1,
         "an old daemon's full welcome: nested objects and arrays around it"},
        {"{\"t\":\"welcome\",\"s\":\"a \\\"t\\\":\\\"ping\\\" b\"}", 1, "an escaped quote in another value does not confuse the scan"},
        {"{\"t\":\"ping\"}", 0, "a ping"},
        {"{\"t\":\"welcomes\"}", 0, "a longer word"},
        {"{\"t\":\"Welcome\"}", 0, "case matters"},
        {"{\"t\":\"welcom\"}", 0, "a shorter word"},
        {"{\"t\":\"\"}", 0, "empty"},
        {"{\"t\":5}", 0, "a number"},
        {"{\"t\":[\"welcome\"]}", 0, "an array"},
        {"{\"t\":null}", 0, "null"},
        {"{\"x\":{\"t\":\"welcome\"}}", 0, "a nested t"},
        {"{\"x\":[{\"t\":\"welcome\"}]}", 0, "a t inside an array"},
        {"{\"x\":\"{\\\"t\\\":\\\"welcome\\\"}\"}", 0, "a t inside a string"},
        {"{\"key\\\"t\":\"welcome\"}", 0, "a key that merely ends the way t does"},
        {"{\"\\u0074\":\"welcome\"}", 0, "an escaped spelling of the key is not read (fail safe)"},
        {"[{\"t\":\"welcome\"}]", 0, "an array at the top"},
        {"\"welcome\"", 0, "a bare string"},
        {"{\"t\":\"welcome\"", 0, "truncated"},
        {"{\"t\":\"welcome\"}x", 0, "trailing text"},
        {"{\"t\":\"welcome\"}{\"t\":\"welcome\"}", 0, "two objects"},
        {"{\"t\":\"welcome\",\"t\":\"ping\"}", 0, "two different t: ambiguous is not a welcome"},
        {"{\"t\":\"ping\",\"t\":\"welcome\"}", 0, "two different t, the welcome last"},
        {"{\"t\":\"welcome\",\"t\":\"welcome\"}", 1, "the same twice"},
        {"{\"t\":\"wel\\u0063ome\"}", 0, "an escaped spelling of the value is not read (fail safe)"},
        {"{\"t\":\"welcome\\n\"}", 0, "an escape inside the value"},
        {"", 0, "empty payload"},
        {"{}", 0, "empty object"},
        {"not json at all", 0, "text"},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        const int got = up_is_welcome((const uint8_t *)t[i].json, strlen(t[i].json));
        if (got != t[i].want) { fprintf(stderr, "welcome detection: %s: got %d want %d\n  %s\n", t[i].why, got, t[i].want, t[i].json); assert(0); }
    }
    // NUL inside, a NULL pointer, nesting past the limit
    static const char nul[] = "{\"t\":\"welcome\",\"x\":\"a\0b\"}";
    assert(!up_is_welcome((const uint8_t *)nul, sizeof nul - 1));
    assert(!up_is_welcome(NULL, 5));
    char deep[200];
    int n = 0;
    deep[n++] = '{';
    memcpy(deep + n, "\"x\":", 4); n += 4;
    for (int k = 0; k < 40; k++) deep[n++] = '[';
    for (int k = 0; k < 40; k++) deep[n++] = ']';
    memcpy(deep + n, ",\"t\":\"welcome\"}", 15); n += 15;
    assert(!up_is_welcome((const uint8_t *)deep, (size_t)n));   // too deep to trust: not a welcome (and no unbounded recursion)
}

static void test_feed_stamps_the_newest_welcome(void)
{
    fresh();
    assert(!g_p.have_welcome);
    feed_json("{\"t\":\"ping\"}", 100);
    assert(!g_p.have_welcome);
    feed_json("{\"t\":\"welcome\",\"probe\":1}", 2100);
    assert(g_p.have_welcome && g_p.last_welcome_ms == 2100);
    feed_json("{\"t\":\"ping\"}", 3000);
    assert(g_p.last_welcome_ms == 2100);                          // other frames do not move it
    feed_json("{\"t\":\"welcome\",\"probe\":1}", 4100);
    assert(g_p.last_welcome_ms == 4100);
}

static void test_only_json_frames_count(void)
{
    fresh();
    uint8_t f[CABLE_MAX_FRAME];
    const char *w = "{\"t\":\"welcome\"}";
    static const uint8_t other[] = {CABLE_TYPE_PCM, CABLE_TYPE_FW, CABLE_TYPE_LOG, CABLE_TYPE_PET, CABLE_TYPE_SHOT, CABLE_TYPE_ADPCM, 0x42};
    for (unsigned i = 0; i < sizeof other; i++) {
        up_core_feed(&g_p, f, frame(other[i], w, strlen(w), f, sizeof f), 500);
        assert(!g_p.have_welcome);   // a log line that spells a welcome is a log line
    }
}

static void test_split_reads_and_noise(void)
{
    fresh();
    uint8_t f[CABLE_MAX_FRAME];
    const size_t n = json_frame("{\"t\":\"welcome\",\"probe\":1}", f, sizeof f);
    // one byte at a time: stamped by the read that completed it
    for (size_t i = 0; i < n; i++) {
        assert(!g_p.have_welcome);
        up_core_feed(&g_p, f + i, 1, 7000 + i);
    }
    assert(g_p.have_welcome && g_p.last_welcome_ms == 7000 + n - 1);
    // bootloader chatter in front of a frame, then a roster burst behind it (an old daemon pushes after a probe hello)
    fresh();
    uint8_t burst[4096];
    size_t len = 0;
    const char *junk = "ESP-ROM:esp32s3-20210327\r\nBuild:Mar 27 2021\r\nrst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\r\n";
    memcpy(burst, junk, strlen(junk));
    len += strlen(junk);
    len += json_frame("{\"t\":\"welcome\",\"proto\":4,\"app\":\"harness\",\"machine\":{\"id\":\"m\",\"name\":\"PC\"},\"features\":[\"voice.draft\",\"settings\"]}", burst + len, sizeof burst - len);
    len += json_frame("{\"t\":\"machines.begin\"}", burst + len, sizeof burst - len);
    for (int k = 0; k < 20; k++) len += json_frame("{\"t\":\"agent\",\"id\":\"abc\",\"name\":\"an agent with a name\"}", burst + len, sizeof burst - len);
    len += json_frame("{\"t\":\"agents.end\",\"total\":20}", burst + len, sizeof burst - len);
    up_core_feed(&g_p, burst, len, 123456);
    assert(g_p.have_welcome && g_p.last_welcome_ms == 123456);
    assert(g_p.dec.corrupt_frames == 0);
}

// The cap. cap = 1,024 bytes of frame: header (6) + payload + CRC (2).
static size_t padded_welcome(char *buf, size_t payload_len)
{
    // {"t":"welcome","app":"xxxx...x"} with the app string sized so the whole payload is exactly payload_len bytes
    const char *head = "{\"t\":\"welcome\",\"app\":\"";
    const char *tail = "\"}";
    const size_t fixed = strlen(head) + strlen(tail);
    assert(payload_len > fixed);
    memcpy(buf, head, strlen(head));
    memset(buf + strlen(head), 'x', payload_len - fixed);
    memcpy(buf + payload_len - strlen(tail), tail, strlen(tail));
    buf[payload_len] = 0;
    return payload_len;
}

static void test_frame_cap(void)
{
    char big[1100];
    uint8_t f[CABLE_MAX_FRAME];
    fresh();
    const size_t max_payload = UP_FRAME_CAP - CABLE_HEADER_BYTES - CABLE_CRC_BYTES;   // 1,016
    padded_welcome(big, max_payload);
    up_core_feed(&g_p, f, frame(CABLE_TYPE_JSON, big, max_payload, f, sizeof f), 10);
    assert(g_p.have_welcome && g_p.last_welcome_ms == 10);                 // exactly the cap: read

    fresh();
    padded_welcome(big, max_payload + 1);
    up_core_feed(&g_p, f, frame(CABLE_TYPE_JSON, big, max_payload + 1, f, sizeof f), 10);
    assert(!g_p.have_welcome);                                              // one byte over: noise, not a welcome

    // an oversized frame does not blind the probe: the next welcome is found after it, in the same read or a later one
    fresh();
    uint8_t two[3000];
    size_t len = frame(CABLE_TYPE_JSON, big, max_payload + 1, two, sizeof two);
    len += json_frame("{\"t\":\"welcome\",\"probe\":1}", two + len, sizeof two - len);
    up_core_feed(&g_p, two, len, 99);
    assert(g_p.have_welcome && g_p.last_welcome_ms == 99);

    // a frame that claims the biggest payload there is (8,192) and never finishes does not hold the decoder past its own cap
    fresh();
    uint8_t claim[16] = {CABLE_MAGIC_0, CABLE_MAGIC_1, CABLE_FRAME_VERSION, CABLE_TYPE_JSON, 0x00, 0x20};   // len 0x2000 = 8192
    up_core_feed(&g_p, claim, 6, 1);
    uint8_t f2[CABLE_MAX_FRAME];
    up_core_feed(&g_p, f2, json_frame("{\"t\":\"welcome\"}", f2, sizeof f2), 2);
    assert(g_p.have_welcome && g_p.last_welcome_ms == 2);
}

static void test_a_new_plug_in_forgets(void)
{
    fresh();
    feed_json("{\"t\":\"welcome\"}", 5000);
    assert(g_p.have_welcome);
    up_core_start(&g_p, 90000);
    assert(!g_p.have_welcome && g_p.last_welcome_ms == 0);
    // half a frame from the last plug-in does not complete into the next one
    uint8_t f[CABLE_MAX_FRAME];
    const size_t n = json_frame("{\"t\":\"welcome\"}", f, sizeof f);
    up_core_feed(&g_p, f, n / 2, 90001);
    up_core_start(&g_p, 91000);
    up_core_feed(&g_p, f + n / 2, n - n / 2, 91001);
    assert(!g_p.have_welcome);
}

static void test_hello_cadence(void)
{
    fresh();
    up_core_start(&g_p, 1000);
    assert(up_core_hello_due(&g_p, 1000));            // at once: the daemon cannot see this dial until it speaks
    assert(!up_core_hello_due(&g_p, 1000));
    assert(!up_core_hello_due(&g_p, 2999));
    assert(up_core_hello_due(&g_p, 3000));            // every 2 s
    assert(!up_core_hello_due(&g_p, 3001));
    // a stall of 10 s is ONE hello, not five
    assert(up_core_hello_due(&g_p, 13000));
    assert(!up_core_hello_due(&g_p, 13001));
    assert(!up_core_hello_due(&g_p, 14999));
    assert(up_core_hello_due(&g_p, 15000));
    // a clock that steps back does not fire a burst
    assert(!up_core_hello_due(&g_p, 100));
    // a new plug-in says hello at once again
    up_core_start(&g_p, 20000);
    assert(up_core_hello_due(&g_p, 20000));
}

static void test_init_refuses_nonsense(void)
{
    up_core_t p;
    uint8_t buf[UP_FRAME_CAP];
    memset(&p, 0, sizeof p);
    assert(!up_core_init(NULL, buf, sizeof buf));
    assert(!up_core_init(&p, NULL, sizeof buf));
    assert(!up_core_init(&p, buf, 4));
    // a bigger buffer does not raise the cap: the frame limit is the probe's, not the buffer's
    static uint8_t big_buf[4096];
    char big[1100];
    uint8_t f[CABLE_MAX_FRAME];
    assert(up_core_init(&p, big_buf, sizeof big_buf));
    up_core_start(&p, 0);
    padded_welcome(big, UP_FRAME_CAP - CABLE_HEADER_BYTES - CABLE_CRC_BYTES + 1);
    up_core_feed(&p, f, frame(CABLE_TYPE_JSON, big, UP_FRAME_CAP - CABLE_HEADER_BYTES - CABLE_CRC_BYTES + 1, f, sizeof f), 5);
    assert(!p.have_welcome);
    memset(&p, 0, sizeof p);
    // a dead probe (init failed) ignores input rather than crashing
    up_core_feed(&p, buf, 4, 0);
    assert(!p.have_welcome);
}

int main(void)
{
    test_hello_frame();
    test_welcome_detection();
    test_feed_stamps_the_newest_welcome();
    test_only_json_frames_count();
    test_split_reads_and_noise();
    test_frame_cap();
    test_a_new_plug_in_forgets();
    test_hello_cadence();
    test_init_refuses_nonsense();
    puts("ok test_usb_probe");
    return 0;
}

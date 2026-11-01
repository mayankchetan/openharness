#include "usb_probe_core.h"

#include <stdio.h>
#include <string.h>

// ── the welcome test ────────────────────────────────────────────────────────────────────────────────
// A small strict scanner over one JSON object. It builds no tree and allocates nothing. It understands strings (with escapes), nesting and the
// scalar tokens just well enough to skip them, and it reads only the top-level key "t". Anything it does not like is "not a welcome". Recursion
// is bounded by MAX_DEPTH, so a hostile payload cannot run the stack out. The daemon's welcome nests three levels at the most (machine{} and
// features[] each holding at most one more); 6 leaves room for that and costs at most 6 frames of a few words each on the mode switch task's
// 4 KB stack.

#define MAX_DEPTH 6

typedef struct { const uint8_t *p, *end; } cur_t;

static void ws(cur_t *c) { while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) c->p++; }

// A string at the opening quote. False if it is not terminated or holds a control byte (a NUL included). `plain` is true when it has no escape;
// then [*start, *start + *len) is its text.
static bool str(cur_t *c, const uint8_t **start, size_t *len, bool *plain)
{
    if (c->p >= c->end || *c->p != '"') return false;
    c->p++;
    *start = c->p;
    *plain = true;
    while (c->p < c->end) {
        const uint8_t b = *c->p;
        if (b == '"') {
            *len = (size_t)(c->p - *start);
            c->p++;
            return true;
        }
        if (b < 0x20) return false;
        if (b == '\\') {
            *plain = false;
            c->p++;
            if (c->p >= c->end) return false;
        }
        c->p++;
    }
    return false;
}

static bool value(cur_t *c, int depth);

// After '{' or '[' has been consumed: the members up to and including the closing bracket.
static bool container(cur_t *c, int depth, bool is_object)
{
    if (depth > MAX_DEPTH) return false;
    const uint8_t close = is_object ? '}' : ']';
    ws(c);
    if (c->p < c->end && *c->p == close) { c->p++; return true; }
    for (;;) {
        ws(c);
        if (is_object) {
            const uint8_t *k;
            size_t n;
            bool plain;
            if (!str(c, &k, &n, &plain)) return false;
            ws(c);
            if (c->p >= c->end || *c->p != ':') return false;
            c->p++;
        }
        if (!value(c, depth + 1)) return false;
        ws(c);
        if (c->p >= c->end) return false;
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == close) { c->p++; return true; }
        return false;
    }
}

static bool value(cur_t *c, int depth)
{
    ws(c);
    if (c->p >= c->end) return false;
    const uint8_t b = *c->p;
    if (b == '"') {
        const uint8_t *s;
        size_t n;
        bool plain;
        return str(c, &s, &n, &plain);
    }
    if (b == '{' || b == '[') {
        c->p++;
        return container(c, depth, b == '{');
    }
    if (b == '}' || b == ']' || b == ',' || b == ':') return false;
    // a number, true, false, null: skipped to the next separator
    const uint8_t *start = c->p;
    while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']' && *c->p != ' ' && *c->p != '\t' && *c->p != '\n' && *c->p != '\r') c->p++;
    return c->p > start;
}

bool up_is_welcome(const uint8_t *payload, size_t len)
{
    if (!payload || len < 2) return false;
    cur_t c = {payload, payload + len};
    ws(&c);
    if (c.p >= c.end || *c.p != '{') return false;
    c.p++;
    int t_seen = 0;
    bool t_is_welcome = true;
    ws(&c);
    if (c.p < c.end && *c.p == '}') return false;     // {}: no "t" at all
    for (;;) {
        ws(&c);
        const uint8_t *key;
        size_t key_len;
        bool key_plain;
        if (!str(&c, &key, &key_len, &key_plain)) return false;
        ws(&c);
        if (c.p >= c.end || *c.p != ':') return false;
        c.p++;
        ws(&c);
        if (key_plain && key_len == 1 && key[0] == 't') {
            // a plain string; a repeated "t" must say the same thing
            const uint8_t *val;
            size_t val_len;
            bool val_plain;
            if (!str(&c, &val, &val_len, &val_plain)) return false;
            const bool is = val_plain && val_len == 7 && memcmp(val, "welcome", 7) == 0;
            if (t_seen && is != t_is_welcome) return false;   // two different "t": ambiguous is not a welcome
            t_is_welcome = is;
            t_seen++;
        } else if (!value(&c, 1)) {
            return false;
        }
        ws(&c);
        if (c.p >= c.end) return false;
        if (*c.p == ',') { c.p++; continue; }
        if (*c.p == '}') { c.p++; break; }
        return false;
    }
    ws(&c);
    return c.p == c.end && t_seen > 0 && t_is_welcome;
}

// ── the probe ───────────────────────────────────────────────────────────────────────────────────────
bool up_core_init(up_core_t *p, uint8_t *buf, size_t cap)
{
    if (!p) return false;
    memset(p, 0, sizeof *p);
    if (!buf || cap < UP_FRAME_CAP) return false;
    return cable_decoder_init_with(&p->dec, buf, UP_FRAME_CAP);
}

void up_core_start(up_core_t *p, uint64_t now_ms)
{
    if (!p || !p->dec.buf) return;
    cable_decoder_buf_reset(&p->dec);
    p->have_welcome = false;
    p->last_welcome_ms = 0;
    p->hello_started = false;
    p->next_hello_ms = now_ms;
    p->now_ms = now_ms;
}

static void on_frame(uint8_t version, uint8_t type, const uint8_t *payload, size_t len, void *ctx)
{
    (void)version;
    up_core_t *p = ctx;
    if (type != CABLE_TYPE_JSON) return;      // a log line that spells a welcome is a log line
    if (up_is_welcome(payload, len)) {
        p->have_welcome = true;
        p->last_welcome_ms = p->now_ms;
    }
}

void up_core_feed(up_core_t *p, const uint8_t *data, size_t n, uint64_t now_ms)
{
    if (!p || !p->dec.buf || !data || !n) return;
    p->now_ms = now_ms;
    cable_decoder_buf_feed(&p->dec, data, n, on_frame, p);
}

bool up_core_pump(up_core_t *p, up_read_fn rd, void *ctx, uint32_t wait_ms, uint64_t now_ms)
{
    if (!p || !p->dec.buf || !rd) return false;
    uint8_t chunk[UP_READ_CHUNK];
    int n = rd(ctx, chunk, sizeof chunk, wait_ms);   // the one read that may wait for the host
    for (unsigned i = 0; n > 0; i++) {
        up_core_feed(p, chunk, (size_t)n, now_ms);
        if (i >= UP_DRAIN_CHUNKS) return true;       // the bound: a host that never stops sending does not keep this task in the loop
        n = rd(ctx, chunk, sizeof chunk, 0);         // drain what is already there, without waiting
    }
    return false;
}

bool up_core_hello_due(up_core_t *p, uint64_t now_ms)
{
    if (!p) return false;
    if (!p->hello_started) {
        p->hello_started = true;
        p->next_hello_ms = now_ms + UP_HELLO_MS;
        return true;
    }
    if (now_ms < p->next_hello_ms) return false;
    p->next_hello_ms = now_ms + UP_HELLO_MS;   // a stall is ONE hello, not a catch-up burst
    return true;
}

size_t up_core_hello(uint8_t *out, size_t cap, int proto, const char *product)
{
    if (!out || !product || proto < 0) return 0;
    for (const char *s = product; *s; s++) if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20) return 0;
    char json[96];
    const int n = snprintf(json, sizeof json, "{\"t\":\"hello\",\"product\":\"%s\",\"proto\":%d,\"probe\":1}", product, proto);
    if (n <= 0 || (size_t)n >= sizeof json) return 0;
    const int f = cable_frame_encode(CABLE_TYPE_JSON, (const uint8_t *)json, (size_t)n, out, cap);
    return f > 0 ? (size_t)f : 0;
}

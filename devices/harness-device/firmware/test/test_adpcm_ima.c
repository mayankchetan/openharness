// Host test for main/adpcm_ima.c against the shared vectors (test/vectors/adpcm_ima.txt), plus properties the
// vectors cannot state: a round trip stays inside the step size, every malformed block is refused without a read
// past the buffer, and the decoder writes exactly n samples and no more.
//
//   cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=undefined,bounds -I main -o t test/test_adpcm_ima.c main/adpcm_ima.c
//   ./t test/vectors/adpcm_ima.txt
//
// The vectors are written by scripts/gen_adpcm_vectors.py from the format text, not from adpcm_ima.c, and the daemon's
// TypeScript codec asserts against the same file.
#include "adpcm_ima.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// Decodes hex up to `end` (exclusive) into a freshly malloc'd buffer of EXACTLY the decoded size, so the address
// sanitizer, where the host has one, sees any read past it. Returns NULL (and *len 0) for an empty string.
static uint8_t *hex_bytes(const char *p, const char *end, size_t *len)
{
    size_t digits = (size_t)(end - p);
    assert(digits % 2 == 0);
    *len = digits / 2;
    if (!*len) return NULL;
    uint8_t *out = malloc(*len);
    assert(out);
    for (size_t i = 0; i < *len; i++) {
        int hi = hex_digit(p[2 * i]), lo = hex_digit(p[2 * i + 1]);
        assert(hi >= 0 && lo >= 0);
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return out;
}
static int16_t *hex_pcm(const char *p, const char *end, size_t *count)
{
    size_t bytes;
    uint8_t *raw = hex_bytes(p, end, &bytes);
    assert(bytes % 2 == 0);
    *count = bytes / 2;
    int16_t *pcm = malloc(*count * sizeof *pcm + 1);
    assert(pcm);
    for (size_t i = 0; i < *count; i++) pcm[i] = (int16_t)(uint16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
    free(raw);
    return pcm;
}
// Splits a line in place on tabs; returns the field count (capped).
static int split(char *line, char **field, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        field[n++] = p;
        char *tab = strchr(p, '\t');
        if (!tab) break;
        *tab = 0;
        p = tab + 1;
    }
    return n;
}
static const char *fend(const char *s) { return s + strlen(s); }

static void run_enc(char **f)
{
    size_t count;
    int16_t *pcm = hex_pcm(f[2], fend(f[2]), &count);
    uint8_t index = (uint8_t)atoi(f[3]);
    size_t expect_len;
    uint8_t *expect = hex_bytes(f[6], fend(f[6]), &expect_len);
    uint8_t out[ADPCM_PAYLOAD_MAX + 1];            // one guard byte past the largest payload
    memset(out, 0xEE, sizeof out);
    int got = adpcm_encode_block(pcm, (unsigned)count, &index, (uint16_t)atoi(f[4]), (uint8_t)atoi(f[5]), out, ADPCM_PAYLOAD_MAX);
    CHECK(got == (int)expect_len, "enc %s: length %d, want %zu", f[1], got, expect_len);
    if (got == (int)expect_len) {
        CHECK(memcmp(out, expect, expect_len) == 0, "enc %s: bytes differ", f[1]);
        CHECK(out[got] == 0xEE, "enc %s: wrote past the payload", f[1]);
    }
    CHECK(index == (uint8_t)atoi(f[7]), "enc %s: index out %d, want %s", f[1], index, f[7]);
    // The bytes the encoder made decode to a block of n samples that starts on the first sample exactly.
    int16_t back[ADPCM_BLOCK_SAMPLES];
    adpcm_info_t info;
    int n = adpcm_decode_block(expect, expect_len, back, ADPCM_BLOCK_SAMPLES, &info);
    CHECK(n == (int)count && back[0] == pcm[0], "enc %s: decode of the vector gave %d samples", f[1], n);
    CHECK(info.index_end == (uint8_t)atoi(f[7]), "enc %s: decoder ends on index %d, want %s", f[1], info.index_end, f[7]);
    free(pcm);
    free(expect);
}

static void run_dec(char **f)
{
    size_t len, count;
    uint8_t *payload = hex_bytes(f[2], fend(f[2]), &len);
    int16_t *expect = hex_pcm(f[3], fend(f[3]), &count);
    // The output buffer is exactly n samples with a canary after it: a decoder that writes the padding samples fails here.
    int16_t *out = malloc((count + 1) * sizeof *out);
    assert(out);
    out[count] = 0x5A5A;
    int n = adpcm_decode_block(payload, len, out, count, NULL);
    CHECK(n == (int)count, "dec %s: %d samples, want %zu", f[1], n, count);
    if (n == (int)count) CHECK(memcmp(out, expect, count * sizeof *out) == 0, "dec %s: samples differ", f[1]);
    CHECK(out[count] == 0x5A5A, "dec %s: wrote past n", f[1]);
    free(payload);
    free(expect);
    free(out);
}

static void run_gap(char **f)
{
    // The firmware never fills gaps (the daemon does); what it owes this line is that every block that arrived is
    // either decoded whole or refused, and that a sequence number it reads back is the one the line carries.
    const char *p = f[2];
    unsigned blocks = 0;
    while (*p) {
        const char *comma = strchr(p, ',');
        const char *end = comma ? comma : fend(p);
        size_t len;
        uint8_t *payload = hex_bytes(p, end, &len);
        int16_t pcm[ADPCM_BLOCK_SAMPLES];
        adpcm_info_t info;
        int n = adpcm_decode_block(payload, len, pcm, ADPCM_BLOCK_SAMPLES, &info);
        CHECK(n > 0 && info.seq == (uint16_t)(payload[0] | payload[1] << 8), "gap %s: block %u refused (%d)", f[1], blocks, n);
        free(payload);
        blocks++;
        p = comma ? comma + 1 : end;
    }
    CHECK(blocks >= 2, "gap %s: only %u blocks", f[1], blocks);
}

static void run_bad(char **f)
{
    size_t len;
    uint8_t *payload = hex_bytes(f[2], fend(f[2]), &len);
    int16_t pcm[ADPCM_BLOCK_SAMPLES];
    memset(pcm, 0x77, sizeof pcm);
    int n = adpcm_decode_block(payload, len, pcm, ADPCM_BLOCK_SAMPLES, NULL);
    CHECK(n < 0, "bad %s: accepted with %d samples", f[1], n);
    free(payload);
}

// ---- properties -------------------------------------------------------------------------------------------

static uint32_t rng = 0xd00e5193;
static uint32_t random_word(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void property_round_trip(void)
{
    // 10^5 random blocks. For every sample the decoder's own step is rebuilt from the payload's nibbles; when the
    // sample was within reach of one code (|s - previous reconstruction| <= 1.5 step), the reconstruction is within
    // one step of it. A block is never worse than a full step where it could do better.
    static const int8_t adjust[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    unsigned worst_ratio_num = 0, in_reach = 0;
    for (unsigned block = 0; block < 100000; block++) {
        unsigned n = 1 + random_word() % ADPCM_BLOCK_SAMPLES;
        int16_t pcm[ADPCM_BLOCK_SAMPLES], back[ADPCM_BLOCK_SAMPLES];
        int kind = (int)(random_word() % 4);
        int walk = (int16_t)(random_word() >> 16);
        for (unsigned i = 0; i < n; i++) {
            if (kind == 0) pcm[i] = (int16_t)(random_word() >> 16);                         // full-scale noise
            else if (kind == 1) { walk += (int)(random_word() % 401) - 200; if (walk > 32767) walk = 32767; if (walk < -32768) walk = -32768; pcm[i] = (int16_t)walk; }
            else if (kind == 2) pcm[i] = (int16_t)(((random_word() >> 16) & 0x3F) - 32);    // quiet noise
            else pcm[i] = (i / 7) % 2 ? 30000 : -30000;                                     // square
        }
        uint8_t index = (uint8_t)(random_word() % (ADPCM_INDEX_MAX + 1)), start = index;
        uint8_t out[ADPCM_PAYLOAD_MAX];
        int len = adpcm_encode_block(pcm, n, &index, (uint16_t)block, 0, out, sizeof out);
        CHECK(len == (int)adpcm_payload_len(n), "round trip: length %d for n=%u", len, n);
        if (len < 0) return;
        adpcm_info_t info;
        int got = adpcm_decode_block(out, (size_t)len, back, ADPCM_BLOCK_SAMPLES, &info);
        CHECK(got == (int)n && info.n == n && info.seq == (uint16_t)block, "round trip: %d samples, want %u", got, n);
        CHECK(info.index_start == start && info.index_end == index, "round trip: index %u..%u, want %u..%u", info.index_start, info.index_end, start, index);
        CHECK(back[0] == pcm[0], "round trip: the first sample is not lossless");
        int idx = start;
        for (unsigned i = 1; i < n && got == (int)n; i++) {
            int code = (out[9 + (i - 1) / 2] >> (((i - 1) & 1) * 4)) & 15;
            int step = adpcm_step_size((unsigned)idx);
            int reach = abs((int)pcm[i] - (int)back[i - 1]);
            if (reach * 2 <= step * 3) {
                in_reach++;
                int err = abs((int)pcm[i] - (int)back[i]);
                if (err > step) { CHECK(0, "round trip: error %d above step %d (block %u sample %u)", err, step, block, i); return; }
                unsigned ratio = (unsigned)(err * 100 / step);
                if (ratio > worst_ratio_num) worst_ratio_num = ratio;
            }
            idx += adjust[code & 7];
            if (idx < 0) idx = 0;
            if (idx > ADPCM_INDEX_MAX) idx = ADPCM_INDEX_MAX;
        }
    }
    CHECK(in_reach > 1000000, "round trip: only %u samples in reach - the property is vacuous", in_reach);
    printf("round trip: 100000 blocks, %u samples in reach, worst error %u%% of a step\n", in_reach, worst_ratio_num);
}

static void rejects_and_bounds(void)
{
    // Every length from 0 to 270 against every interesting n, in an exactly-sized buffer: only the one right length
    // is accepted, and none reads past it.
    static const unsigned counts[] = {0, 1, 2, 8, 9, 10, 17, 100, 504, 505, 506, 1000, 65535};
    for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; c++) {
        for (size_t len = 0; len <= 270; len++) {
            uint8_t *p = len ? malloc(len) : NULL;
            if (len) memset(p, 0, len);
            if (len >= 4) { p[2] = (uint8_t)(counts[c] & 0xFF); p[3] = (uint8_t)(counts[c] >> 8); }
            int16_t pcm[ADPCM_BLOCK_SAMPLES];
            int n = adpcm_decode_block(p, len, pcm, ADPCM_BLOCK_SAMPLES, NULL);
            int valid = counts[c] >= 1 && counts[c] <= 505 && len == adpcm_payload_len(counts[c]);
            CHECK((n > 0) == valid && (!valid || n == (int)counts[c]), "n=%u len=%zu -> %d", counts[c], len, n);
            free(p);
        }
    }
    // The step index.
    for (unsigned index = 80; index < 256; index++) {
        uint8_t p[13] = {0, 0, 9, 0, 0, 0, 0, (uint8_t)index, 0, 0x11, 0x11, 0x11, 0x11};
        int16_t pcm[16];
        int n = adpcm_decode_block(p, sizeof p, pcm, 16, NULL);
        CHECK((n > 0) == (index <= ADPCM_INDEX_MAX), "index %u -> %d", index, n);
    }
    // The caller's buffer is too small for n: refused, nothing written.
    uint8_t good[ADPCM_PAYLOAD_MAX];
    int16_t in[ADPCM_BLOCK_SAMPLES] = {0}, small[3] = {7, 7, 7};
    uint8_t index = 0;
    int len = adpcm_encode_block(in, ADPCM_BLOCK_SAMPLES, &index, 0, 0, good, sizeof good);
    CHECK(len == ADPCM_PAYLOAD_MAX, "full block is %d bytes", len);
    CHECK(adpcm_decode_block(good, (size_t)len, small, 3, NULL) == ADPCM_ERR_CAP && small[0] == 7 && small[2] == 7, "small output buffer");
    // Encoder argument checks: n = 0, n too large, a step index out of range, a buffer one byte short.
    index = 0;
    CHECK(adpcm_encode_block(in, 0, &index, 0, 0, good, sizeof good) < 0, "encode n=0");
    CHECK(adpcm_encode_block(in, 506, &index, 0, 0, good, sizeof good) < 0, "encode n=506");
    index = 89;
    CHECK(adpcm_encode_block(in, 10, &index, 0, 0, good, sizeof good) < 0, "encode index 89");
    index = 0;
    CHECK(adpcm_encode_block(in, 505, &index, 0, 0, good, ADPCM_PAYLOAD_MAX - 1) < 0, "encode into a short buffer");
    CHECK(adpcm_encode_block(NULL, 5, &index, 0, 0, good, sizeof good) < 0, "encode NULL pcm");
    CHECK(adpcm_payload_len(0) == 0 && adpcm_payload_len(506) == 0 && adpcm_payload_len(1) == 9 && adpcm_payload_len(505) == 261 &&
          adpcm_payload_len(2) == 13 && adpcm_payload_len(9) == 13 && adpcm_payload_len(10) == 17, "payload length table");
    CHECK(adpcm_step_size(0) == 7 && adpcm_step_size(88) == 32767 && adpcm_step_size(89) == 0, "step table ends");
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "test/vectors/adpcm_ima.txt";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    assert(text && fread(text, 1, (size_t)size, f) == (size_t)size);
    text[size] = 0;
    fclose(f);

    unsigned enc = 0, dec = 0, gap = 0, bad = 0;
    for (char *line = text, *next; *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0; else next = line + strlen(line);
        size_t len = strlen(line);
        while (len && line[len - 1] == '\r') line[--len] = 0;      // a CRLF checkout reads the same
        if (!len || line[0] == '#') continue;
        char *field[8];
        int n = split(line, field, 8);
        if (n == 8 && !strcmp(field[0], "enc")) { run_enc(field); enc++; }
        else if (n == 4 && !strcmp(field[0], "dec")) { run_dec(field); dec++; }
        else if (n == 4 && !strcmp(field[0], "gap")) { run_gap(field); gap++; }
        else if (n == 3 && !strcmp(field[0], "bad")) { run_bad(field); bad++; }
        else { fprintf(stderr, "unreadable vector line: %s (%d fields)\n", field[0], n); failures++; }
    }
    free(text);
    CHECK(enc >= 20 && dec >= 8 && gap >= 5 && bad >= 10, "vector file is thin: enc %u dec %u gap %u bad %u", enc, dec, gap, bad);
    printf("vectors: %s - enc %u, dec %u, gap %u, bad %u\n", path, enc, dec, gap, bad);

    rejects_and_bounds();
    property_round_trip();
    printf("%s: %d checks, %d failure(s)\n", failures ? "FAILED" : "ok", checks, failures);
    return failures ? 1 : 0;
}

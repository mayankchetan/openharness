// battery_ring.c: the NVS ring of battery samples, against an in-memory store. 24 blocks of 60 entries (1,440), one blob a
// block, a meta record; the blob is the truth about its own length (a crash between the two writes repairs itself). Entries are
// 8 bytes (boot, uptime, percent, millivolts); a ring of another version (the 6-byte entries of the first image) is dropped.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "battery_ring.h"

enum { BYTES = BATLOG_ENTRY_BYTES * BATLOG_BLOCK };
static struct { uint8_t data[BYTES]; size_t len; bool present; } blocks[BATLOG_BLOCKS];
static batlog_meta_t meta_store;
static bool meta_present;
static int saves, meta_saves;
static bool fail_save;

static bool load(int block, uint8_t *buf, size_t *len)
{
    if (!blocks[block].present) { *len = 0; return true; }
    memcpy(buf, blocks[block].data, blocks[block].len);
    *len = blocks[block].len;
    return true;
}
static bool save(int block, const uint8_t *buf, size_t len)
{
    if (fail_save) return false;
    memcpy(blocks[block].data, buf, len);
    blocks[block].len = len; blocks[block].present = true; saves++;
    return true;
}
static bool load_meta(batlog_meta_t *m) { if (!meta_present) return false; *m = meta_store; return true; }
static bool save_meta(const batlog_meta_t *m) { meta_store = *m; meta_present = true; meta_saves++; return true; }
static const batlog_store_t store = {load, save, load_meta, save_meta};

static batlog_entry_t entry(uint16_t boot, uint32_t up, uint8_t pct) { return (batlog_entry_t){boot, up, pct, (uint16_t)(3000 + pct * 12)}; }

static int dumped;
static batlog_entry_t seen[2000];
static uint32_t seqs[2000];
static void emit(uint32_t seq, const batlog_entry_t *e, void *ctx) { (void)ctx; seqs[dumped] = seq; seen[dumped++] = *e; }

int main(void)
{
    // Encoding: 8 bytes, little endian, uptime in 24 bits (saturating), then the percent and the millivolts.
    uint8_t raw[BATLOG_ENTRY_BYTES];
    batlog_encode(raw, &(batlog_entry_t){0x1234, 0x00ABCDEF, 87, 4012});
    assert(raw[0] == 0x34 && raw[1] == 0x12 && raw[2] == 0xEF && raw[3] == 0xCD && raw[4] == 0xAB && raw[5] == 87 &&
           raw[6] == (4012 & 0xFF) && raw[7] == (4012 >> 8));
    batlog_entry_t back;
    batlog_decode(raw, &back);
    assert(back.boot == 0x1234 && back.up_s == 0x00ABCDEF && back.pct == 87 && back.mv == 4012);
    batlog_encode(raw, &(batlog_entry_t){1, 0xFFFFFFFFu, 5, 0});
    batlog_decode(raw, &back);
    assert(back.up_s == 0x00FFFFFF);
    batlog_encode(raw, &(batlog_entry_t){1, 0x01000005u, 5, 0});   // saturates, never wraps to a small number
    batlog_decode(raw, &back);
    assert(back.up_s == 0x00FFFFFF);
    assert(BATLOG_ENTRY_BYTES == 8 && BATLOG_BLOCK == 60 && BATLOG_BLOCKS == 24 && BATLOG_MAX == 1440 && BATLOG_VERSION == 2);
    assert(sizeof(batlog_meta_t) == 12);   // the same record size as version 1: the version sits in what was padding

    // Boot numbers count up from 1 and persist.
    assert(batlog_begin_boot(&store) == 1 && batlog_begin_boot(&store) == 2);

    // 130 appends: blocks 0 and 1 full, block 2 holds 10; a dump lists all 130 in order and only once.
    for (int i = 0; i < 130; i++) assert(batlog_append(&store, entry(2, 60u * (unsigned)i, (uint8_t)(100 - i / 2))));
    assert(blocks[0].len == BYTES && blocks[1].len == BYTES && blocks[2].len == 10 * BATLOG_ENTRY_BYTES && !blocks[3].present);
    assert(batlog_dump(&store, emit, NULL) == 130 && dumped == 130);
    for (int i = 0; i < 130; i++) assert(seqs[i] == (uint32_t)i && seen[i].up_s == 60u * (unsigned)i && seen[i].boot == 2 && seen[i].mv == 3000 + seen[i].pct * 12);
    dumped = 0;
    assert(batlog_dump(&store, emit, NULL) == 0 && dumped == 0);   // already dumped
    for (int i = 0; i < 5; i++) batlog_append(&store, entry(3, 1000u + (unsigned)i, 50));
    assert(batlog_dump(&store, emit, NULL) == 5 && seqs[0] == 130 && seen[0].boot == 3 && seen[4].up_s == 1004);

    // Wrap: past 1,440 entries a new block overwrites the oldest one, so the ring keeps the newest 1,380 to 1,440 entries
    // (23 whole blocks and the part of the current one), oldest first, in sequence.
    batlog_begin_boot(&store);
    for (int i = 0; i < 3000; i++) batlog_append(&store, entry(4, (uint32_t)i, (uint8_t)(i % 101)));
    assert(meta_store.total == 3135);
    dumped = 0;
    int n = batlog_dump(&store, emit, NULL);
    assert(n == 3135 - 1740 && dumped == n);   // total / 60 = 52 blocks begun: the oldest kept starts at (52 - 23) * 60
    assert(seqs[0] == 1740 && seqs[n - 1] == 3134);
    for (int i = 1; i < n; i++) assert(seqs[i] == seqs[i - 1] + 1 && seen[i].up_s == seen[i - 1].up_s + 1);
    assert(seen[n - 1].up_s == 2999 && seen[n - 1].pct == 2999 % 101 && seen[n - 1].boot == 4);

    // A write that fails changes nothing (no meta advance): the same sample can be tried again.
    uint32_t total_before = meta_store.total;
    fail_save = true;
    assert(!batlog_append(&store, entry(4, 1, 1)));
    fail_save = false;
    assert(meta_store.total == total_before);

    // A crash between the blob write and the meta write: the blob is one entry ahead of the meta. The next append repairs it.
    {
        batlog_meta_t m = meta_store;
        int block = (int)((m.total / BATLOG_BLOCK) % BATLOG_BLOCKS);
        if (m.total % BATLOG_BLOCK == 0) { /* start of a block: make the ahead entry the first of it */ }
        uint8_t rawe[BATLOG_ENTRY_BYTES];
        batlog_encode(rawe, &(batlog_entry_t){9, 777, 33, 0});
        size_t at = blocks[block].present ? blocks[block].len : 0;
        if (m.total % BATLOG_BLOCK == 0) at = 0;
        memcpy(blocks[block].data + at, rawe, sizeof rawe);
        blocks[block].len = at + sizeof rawe; blocks[block].present = true;   // meta not advanced
        assert(batlog_append(&store, entry(9, 778, 32)));
        assert(meta_store.total == m.total + 2);   // both entries counted
        dumped = 0;
        batlog_dump(&store, emit, NULL);
        assert(dumped >= 2 && seen[dumped - 2].up_s == 777 && seen[dumped - 1].up_s == 778);
    }

    // The first image's ring (6-byte entries, meta without a version) is dropped at the next boot, not misread.
    {
        memset(blocks, 0, sizeof blocks); memset(&meta_store, 0, sizeof meta_store); meta_present = false; saves = 0; dumped = 0;
        // Fresh device: no meta, nothing to erase.
        assert(batlog_begin_boot(&store) == 1 && saves == 0 && meta_store.version == BATLOG_VERSION);
        // An old ring: 188 samples in four blocks of 6-byte entries (the last one 8 samples), boot 3, all dumped.
        memset(blocks, 0, sizeof blocks);
        for (int b = 0; b < 4; b++) { memset(blocks[b].data, 0x5A, sizeof blocks[b].data); blocks[b].len = (b == 3 ? 8 : 60) * 6; blocks[b].present = true; }
        meta_store = (batlog_meta_t){.total = 188, .dumped = 188, .boot = 3, .version = 0};
        assert(batlog_append(&store, entry(3, 1, 1)) == false);          // before the boot step the old ring is not touched
        assert(batlog_dump(&store, emit, NULL) == 0 && dumped == 0);     // and not read either
        assert(batlog_begin_boot(&store) == 4);                          // the boot count carries on
        assert(meta_store.version == BATLOG_VERSION && meta_store.total == 0 && meta_store.dumped == 0 && meta_store.boot == 4);
        for (int b = 0; b < BATLOG_BLOCKS; b++) assert(blocks[b].len == 0);
        for (int i = 0; i < 3; i++) assert(batlog_append(&store, entry(4, 60u * (unsigned)i, (uint8_t)(90 - i))));
        assert(batlog_dump(&store, emit, NULL) == 3 && dumped == 3);
        assert(seqs[0] == 0 && seen[0].boot == 4 && seen[2].up_s == 120 && seen[2].pct == 88 && seen[2].mv == 3000 + 88 * 12);
        // An erase that fails keeps the old version, so the next boot tries again; nothing is appended meanwhile.
        meta_store = (batlog_meta_t){.total = 188, .dumped = 188, .boot = 3, .version = 0};
        for (int b = 0; b < 4; b++) { blocks[b].len = 60 * 6; blocks[b].present = true; }
        fail_save = true;
        assert(batlog_begin_boot(&store) == 0 && meta_store.version == 0 && meta_store.boot == 3);
        fail_save = false;
        assert(!batlog_append(&store, entry(3, 1, 1)));
        assert(batlog_begin_boot(&store) == 4 && meta_store.version == BATLOG_VERSION);
    }

    // The percent stored is the gauge's own (0..100); the caller decides what to log.
    puts("battery ring: 8-byte entries with mV, 24 x 60 blocks, order across the wrap, dump once, crash repair, old ring dropped PASS");
    return 0;
}

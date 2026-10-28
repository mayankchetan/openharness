// battery_ring.c: the NVS ring of battery samples, against an in-memory store. 24 blocks of 60 entries (1,440), one blob a
// block, a meta record; the blob is the truth about its own length (a crash between the two writes repairs itself).
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

static batlog_entry_t entry(uint16_t boot, uint32_t up, uint8_t pct) { return (batlog_entry_t){boot, up, pct}; }

static int dumped;
static batlog_entry_t seen[2000];
static uint32_t seqs[2000];
static void emit(uint32_t seq, const batlog_entry_t *e, void *ctx) { (void)ctx; seqs[dumped] = seq; seen[dumped++] = *e; }

int main(void)
{
    // Encoding: 6 bytes, little endian, uptime in 24 bits (saturating).
    uint8_t raw[BATLOG_ENTRY_BYTES];
    batlog_encode(raw, &(batlog_entry_t){0x1234, 0x00ABCDEF, 87});
    assert(raw[0] == 0x34 && raw[1] == 0x12 && raw[2] == 0xEF && raw[3] == 0xCD && raw[4] == 0xAB && raw[5] == 87);
    batlog_entry_t back;
    batlog_decode(raw, &back);
    assert(back.boot == 0x1234 && back.up_s == 0x00ABCDEF && back.pct == 87);
    batlog_encode(raw, &(batlog_entry_t){1, 0xFFFFFFFFu, 5});
    batlog_decode(raw, &back);
    assert(back.up_s == 0x00FFFFFF);
    batlog_encode(raw, &(batlog_entry_t){1, 0x01000005u, 5});   // saturates, never wraps to a small number
    batlog_decode(raw, &back);
    assert(back.up_s == 0x00FFFFFF);
    assert(BATLOG_ENTRY_BYTES == 6 && BATLOG_BLOCK == 60 && BATLOG_BLOCKS == 24 && BATLOG_MAX == 1440);

    // Boot numbers count up from 1 and persist.
    assert(batlog_begin_boot(&store) == 1 && batlog_begin_boot(&store) == 2);

    // 130 appends: blocks 0 and 1 full, block 2 holds 10; a dump lists all 130 in order and only once.
    for (int i = 0; i < 130; i++) assert(batlog_append(&store, entry(2, 60u * (unsigned)i, (uint8_t)(100 - i / 2))));
    assert(blocks[0].len == BYTES && blocks[1].len == BYTES && blocks[2].len == 10 * BATLOG_ENTRY_BYTES && !blocks[3].present);
    assert(batlog_dump(&store, emit, NULL) == 130 && dumped == 130);
    for (int i = 0; i < 130; i++) assert(seqs[i] == (uint32_t)i && seen[i].up_s == 60u * (unsigned)i && seen[i].boot == 2);
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
        batlog_encode(rawe, &(batlog_entry_t){9, 777, 33});
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

    // Percent is stored as given (0..100); the caller decides what to log.
    puts("battery ring: 6-byte entries, 24 x 60 blocks, order across the wrap, dump once, crash repair PASS");
    return 0;
}

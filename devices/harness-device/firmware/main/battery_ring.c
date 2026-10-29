#include "battery_ring.h"

#include <string.h>

enum { BLOCK_BYTES = BATLOG_BLOCK * BATLOG_ENTRY_BYTES };

void batlog_encode(uint8_t out[BATLOG_ENTRY_BYTES], const batlog_entry_t *e)
{
    uint32_t up = e->up_s > 0x00FFFFFFu ? 0x00FFFFFFu : e->up_s;
    out[0] = (uint8_t)e->boot; out[1] = (uint8_t)(e->boot >> 8);
    out[2] = (uint8_t)up; out[3] = (uint8_t)(up >> 8); out[4] = (uint8_t)(up >> 16);
    out[5] = e->pct;
    out[6] = (uint8_t)e->mv; out[7] = (uint8_t)(e->mv >> 8);
}
void batlog_decode(const uint8_t in[BATLOG_ENTRY_BYTES], batlog_entry_t *e)
{
    e->boot = (uint16_t)(in[0] | in[1] << 8);
    e->up_s = (uint32_t)in[2] | (uint32_t)in[3] << 8 | (uint32_t)in[4] << 16;
    e->pct = in[5];
    e->mv = (uint16_t)(in[6] | in[7] << 8);
}

_Static_assert(sizeof(batlog_meta_t) == 12, "the meta record keeps version 1's size, so its blob loads under both");

// True when a record was read (a missing one is zeros).
static bool meta_get(const batlog_store_t *st, batlog_meta_t *m)
{
    memset(m, 0, sizeof *m);
    return st->load_meta && st->load_meta(m);
}

uint16_t batlog_begin_boot(const batlog_store_t *st)
{
    batlog_meta_t m;
    const bool had = meta_get(st, &m);
    if (m.version != BATLOG_VERSION) {
        // Another format (the 6-byte entries of the first image, or a damaged record): its entries cannot be read with this
        // size. Empty every block so none is mistaken for a sample, and start the counts again; the boot counter carries on.
        // A device with no record at all has no blocks either.
        if (had) {
            const uint8_t none = 0;
            for (int block = 0; block < BATLOG_BLOCKS; block++)
                if (!st->save(block, &none, 0)) return 0;   // the version stays old, so the next boot tries again
        }
        m.total = 0; m.dumped = 0; m.version = BATLOG_VERSION;
    }
    m.boot++;
    return st->save_meta(&m) ? m.boot : 0;
}

bool batlog_append(const batlog_store_t *st, batlog_entry_t e)
{
    batlog_meta_t m;
    meta_get(st, &m);
    if (m.version != BATLOG_VERSION) return false;   // begin_boot has not (successfully) run on this ring
    uint8_t buf[BLOCK_BYTES];
    size_t len = 0;
    unsigned idx = m.total % BATLOG_BLOCK;
    int block = (int)((m.total / BATLOG_BLOCK) % BATLOG_BLOCKS);
    if (!st->load(block, buf, &len)) return false;
    unsigned have = (unsigned)(len / BATLOG_ENTRY_BYTES);
    if (have > BATLOG_BLOCK) have = BATLOG_BLOCK;
    if (have == idx + 1) { m.total++; idx++; }                       // a crash left the blob one ahead: count that sample
    else if (have < idx) { m.total -= idx - have; idx = have; }      // samples lost: continue after the last one kept
    // else the blob is the block's previous lap (idx 0) or exact: keep the first idx samples
    batlog_encode(buf + idx * BATLOG_ENTRY_BYTES, &e);
    if (!st->save(block, buf, (idx + 1) * BATLOG_ENTRY_BYTES)) return false;
    m.total++;
    return st->save_meta(&m);
}

int batlog_dump(const batlog_store_t *st, batlog_emit_t emit, void *ctx)
{
    batlog_meta_t m;
    meta_get(st, &m);
    if (m.version != BATLOG_VERSION) return 0;
    uint32_t blocks_begun = m.total / BATLOG_BLOCK;
    uint32_t lowest = blocks_begun >= BATLOG_BLOCKS ? (blocks_begun - (BATLOG_BLOCKS - 1)) * BATLOG_BLOCK : 0;
    uint32_t first = m.dumped > lowest ? m.dumped : lowest;
    uint8_t buf[BLOCK_BYTES];
    size_t len = 0;
    int loaded = -1, count = 0;
    for (uint32_t seq = first; seq < m.total; seq++) {
        int block = (int)((seq / BATLOG_BLOCK) % BATLOG_BLOCKS);
        if (block != loaded) {
            if (!st->load(block, buf, &len)) return -1;
            loaded = block;
        }
        unsigned idx = seq % BATLOG_BLOCK;
        if (idx >= len / BATLOG_ENTRY_BYTES) continue;
        batlog_entry_t e;
        batlog_decode(buf + idx * BATLOG_ENTRY_BYTES, &e);
        emit(seq, &e, ctx);
        count++;
    }
    m.dumped = m.total;
    if (!st->save_meta(&m)) return -1;
    return count;
}

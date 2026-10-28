#include "battery_ring.h"

#include <string.h>

enum { BLOCK_BYTES = BATLOG_BLOCK * BATLOG_ENTRY_BYTES };

void batlog_encode(uint8_t out[BATLOG_ENTRY_BYTES], const batlog_entry_t *e)
{
    uint32_t up = e->up_s > 0x00FFFFFFu ? 0x00FFFFFFu : e->up_s;
    out[0] = (uint8_t)e->boot; out[1] = (uint8_t)(e->boot >> 8);
    out[2] = (uint8_t)up; out[3] = (uint8_t)(up >> 8); out[4] = (uint8_t)(up >> 16);
    out[5] = e->pct;
}
void batlog_decode(const uint8_t in[BATLOG_ENTRY_BYTES], batlog_entry_t *e)
{
    e->boot = (uint16_t)(in[0] | in[1] << 8);
    e->up_s = (uint32_t)in[2] | (uint32_t)in[3] << 8 | (uint32_t)in[4] << 16;
    e->pct = in[5];
}

static void meta_get(const batlog_store_t *st, batlog_meta_t *m)
{
    memset(m, 0, sizeof *m);
    if (st->load_meta) st->load_meta(m);
}

uint16_t batlog_begin_boot(const batlog_store_t *st)
{
    batlog_meta_t m;
    meta_get(st, &m);
    m.boot++;
    return st->save_meta(&m) ? m.boot : 0;
}

bool batlog_append(const batlog_store_t *st, batlog_entry_t e)
{
    batlog_meta_t m;
    meta_get(st, &m);
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

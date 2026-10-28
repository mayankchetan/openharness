#pragma once
// The battery sample ring (DEVICE_BATTERY_LOG, battery_log.c): up to 1,440 samples, one a minute, kept in 24 blocks of 60.
// One block is one blob, so a sample rewrites its block's blob (at most 360 B) and a meta record. This file is the logic
// with the storage behind four functions, so a host test can run it against memory; battery_log.c plugs in NVS.
//
// A sample is {boot, up_s, pct}: the boot number (a counter kept here) makes uptimes from different boots readable.
// The blob is the truth about its own length: a crash between the blob write and the meta write leaves the blob one
// sample ahead, and the next append counts it.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BATLOG_BLOCK 60
#define BATLOG_BLOCKS 24
#define BATLOG_MAX (BATLOG_BLOCK * BATLOG_BLOCKS)
#define BATLOG_ENTRY_BYTES 6

typedef struct { uint16_t boot; uint32_t up_s; uint8_t pct; } batlog_entry_t;   // up_s is kept in 24 bits (194 days)
typedef struct { uint32_t total; uint32_t dumped; uint16_t boot; } batlog_meta_t;

typedef struct {
    bool (*load)(int block, uint8_t *buf, size_t *len);          // a missing block is len 0 and true
    bool (*save)(int block, const uint8_t *buf, size_t len);
    bool (*load_meta)(batlog_meta_t *);                          // false: none yet (zeros are used)
    bool (*save_meta)(const batlog_meta_t *);
} batlog_store_t;

void batlog_encode(uint8_t out[BATLOG_ENTRY_BYTES], const batlog_entry_t *e);
void batlog_decode(const uint8_t in[BATLOG_ENTRY_BYTES], batlog_entry_t *e);

// Count a boot: returns its number (1, 2, ...), stored in the meta record. 0 on a storage failure.
uint16_t batlog_begin_boot(const batlog_store_t *);
// Append one sample. False (and nothing advanced) when the storage fails.
bool batlog_append(const batlog_store_t *, batlog_entry_t);
// Call emit(seq, entry, ctx) for every kept sample not yet dumped, oldest first, then mark them dumped. Returns the count,
// -1 on a storage failure (nothing marked).
typedef void (*batlog_emit_t)(uint32_t seq, const batlog_entry_t *, void *);
int batlog_dump(const batlog_store_t *, batlog_emit_t emit, void *ctx);

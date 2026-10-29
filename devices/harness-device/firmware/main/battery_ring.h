#pragma once
// The battery sample ring (DEVICE_BATTERY_LOG, battery_log.c): up to 1,440 samples, one a minute, kept in 24 blocks of 60.
// One block is one blob, so a sample rewrites its block's blob (at most 480 B) and a meta record. This file is the logic
// with the storage behind four functions, so a host test can run it against memory; battery_log.c plugs in NVS.
//
// A sample is {boot, up_s, pct, mv}: the boot number (a counter kept here) makes uptimes from different boots readable;
// pct is the fuel gauge's own percent and mv the cell voltage in mV (0 = not read).
// The blob is the truth about its own length: a crash between the blob write and the meta write leaves the blob one
// sample ahead, and the next append counts it.
//
// VERSION. The meta record carries the format version. Version 1 (the first image) had 6-byte entries {boot, up_s, pct} and no
// version field (it reads as 0). A ring of another version is dropped at the next boot (batlog_begin_boot empties every block,
// keeps the boot counter) instead of being read with the wrong entry size.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BATLOG_BLOCK 60
#define BATLOG_BLOCKS 24
#define BATLOG_MAX (BATLOG_BLOCK * BATLOG_BLOCKS)
#define BATLOG_ENTRY_BYTES 8
#define BATLOG_VERSION 2

typedef struct { uint16_t boot; uint32_t up_s; uint8_t pct; uint16_t mv; } batlog_entry_t;   // up_s is kept in 24 bits (194 days)
// 12 bytes, as in version 1: `version` takes the place of what was padding there.
typedef struct { uint32_t total; uint32_t dumped; uint16_t boot; uint16_t version; } batlog_meta_t;

typedef struct {
    bool (*load)(int block, uint8_t *buf, size_t *len);          // a missing block is len 0 and true
    bool (*save)(int block, const uint8_t *buf, size_t len);     // len 0: erase the block
    bool (*load_meta)(batlog_meta_t *);                          // false: none yet (zeros are used)
    bool (*save_meta)(const batlog_meta_t *);
} batlog_store_t;

void batlog_encode(uint8_t out[BATLOG_ENTRY_BYTES], const batlog_entry_t *e);
void batlog_decode(const uint8_t in[BATLOG_ENTRY_BYTES], batlog_entry_t *e);

// Count a boot: returns its number (1, 2, ...), stored in the meta record. 0 on a storage failure. A ring of another version
// is emptied first (and its counters reset); if that fails the version stays old, 0 is returned, and the next boot tries again.
uint16_t batlog_begin_boot(const batlog_store_t *);
// Append one sample. False (and nothing advanced) when the storage fails or the ring is not of this version (no boot step yet).
bool batlog_append(const batlog_store_t *, batlog_entry_t);
// Call emit(seq, entry, ctx) for every kept sample not yet dumped, oldest first, then mark them dumped. Returns the count,
// -1 on a storage failure (nothing marked); 0 for a ring of another version.
typedef void (*batlog_emit_t)(uint32_t seq, const batlog_entry_t *, void *);
int batlog_dump(const batlog_store_t *, batlog_emit_t emit, void *ctx);

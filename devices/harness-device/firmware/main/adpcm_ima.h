#pragma once
// IMA ADPCM voice blocks for cable frame type 0x07 (CABLE_TYPE_ADPCM). IDF-free on purpose, like cable_frame.h: the
// host tests compile this file with a plain compiler, and the vectors in test/vectors/adpcm_ima.txt (written by
// scripts/gen_adpcm_vectors.py from docs/specs/cable-wifi-link.md, section 2) are what it is checked against.
//
// One payload = a 5-byte prefix (seq u16le, n u16le, flags u8) + one WAV IMA ADPCM mono block:
//   pred s16le (sample 0, verbatim) | index u8 (0..88) | reserved u8 | nibbles, low first, 8 samples per 4 bytes.
// A full block is n = 505 samples in 261 bytes. Every block restarts from its own pred and index, so a lost frame costs
// 505 samples and nothing more.
#include <stddef.h>
#include <stdint.h>

#define ADPCM_BLOCK_SAMPLES 505   // (256 - 4) * 2 + 1: the samples in a 256-byte WAV IMA mono block
#define ADPCM_PREFIX_BYTES  5
#define ADPCM_PAYLOAD_MAX   261   // 5 + 4 + 4 * ceil(504 / 8)
#define ADPCM_INDEX_MAX     88
#define ADPCM_FLAG_LAST     0x01  // bit 0: the last block of the turn (diagnostic only; receivers do not depend on it)

// Negative returns. Nothing is written to the caller's output on any of them.
enum {
    ADPCM_ERR_SHORT  = -1,   // fewer than ADPCM_PREFIX_BYTES bytes, or a NULL payload
    ADPCM_ERR_COUNT  = -2,   // n is 0 or above ADPCM_BLOCK_SAMPLES
    ADPCM_ERR_LENGTH = -3,   // the length is not 5 + 4 + 4 * ceil((n - 1) / 8)
    ADPCM_ERR_INDEX  = -4,   // the step index is above ADPCM_INDEX_MAX
    ADPCM_ERR_CAP    = -5,   // the caller's buffer is too small
    ADPCM_ERR_ARG    = -6,   // a NULL pointer, or an index_io out of range
};

typedef struct {
    uint16_t seq;
    uint16_t n;
    uint8_t flags;
    uint8_t index_start;   // the step index at sample 0, as written in the header
    uint8_t index_end;     // the step index after the last real code (not after the zero pad nibbles)
} adpcm_info_t;

// Payload bytes for a block of n samples, or 0 when n is outside 1..ADPCM_BLOCK_SAMPLES.
size_t adpcm_payload_len(unsigned n);

// Encode n samples (1..505). `*index_io` is the step index to start from and is replaced by the index the NEXT block
// should start from, so the caller carries it across a turn; start a turn at 0. Writes payload_len(n) bytes and
// returns that count, or a negative ADPCM_ERR_* (and leaves *index_io alone) on a bad argument.
int adpcm_encode_block(const int16_t *pcm, unsigned n, uint8_t *index_io, uint16_t seq, uint8_t flags,
                       uint8_t *out, size_t out_cap);

// Decode one payload into pcm (at most pcm_cap samples). Returns n, the number of samples written, or a negative
// ADPCM_ERR_*. Exactly n samples are written, never the zero-padded tail. `info` may be NULL.
int adpcm_decode_block(const uint8_t *payload, size_t len, int16_t *pcm, size_t pcm_cap, adpcm_info_t *info);

// The IMA step size at an index (7 .. 32767), or 0 for an index above ADPCM_INDEX_MAX. For tests and diagnostics.
int adpcm_step_size(unsigned index);

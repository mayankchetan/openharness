#include "adpcm_ima.h"

// The standard IMA/DVI step table and index adjustment.
static const uint16_t STEP[ADPCM_INDEX_MAX + 1] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
    658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t ADJUST[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

int adpcm_step_size(unsigned index) { return index <= ADPCM_INDEX_MAX ? STEP[index] : 0; }

size_t adpcm_payload_len(unsigned n)
{
    if (n < 1 || n > ADPCM_BLOCK_SAMPLES) return 0;
    return ADPCM_PREFIX_BYTES + 4 + 4 * (size_t)((n - 1 + 7) / 8);
}

// One code: the shifted-sum form of the IMA rule (not the one-multiply form some libraries use), so the vectors and
// the daemon agree sample for sample. See docs/specs/cable-wifi-link.md section 2.2.
static void advance(int *pred, int *index, unsigned code)
{
    const int step = STEP[*index];
    int diff = step >> 3;
    if (code & 1) diff += step >> 2;
    if (code & 2) diff += step >> 1;
    if (code & 4) diff += step;
    int p = (code & 8) ? *pred - diff : *pred + diff;
    if (p > 32767) p = 32767;
    if (p < -32768) p = -32768;
    int i = *index + ADJUST[code & 7];
    if (i < 0) i = 0;
    if (i > ADPCM_INDEX_MAX) i = ADPCM_INDEX_MAX;
    *pred = p;
    *index = i;
}

int adpcm_encode_block(const int16_t *pcm, unsigned n, uint8_t *index_io, uint16_t seq, uint8_t flags, uint8_t tag,
                       uint8_t *out, size_t out_cap)
{
    if (!pcm || !index_io || !out) return ADPCM_ERR_ARG;
    if (*index_io > ADPCM_INDEX_MAX) return ADPCM_ERR_ARG;
    const size_t len = adpcm_payload_len(n);
    if (!len) return ADPCM_ERR_COUNT;
    if (out_cap < len) return ADPCM_ERR_CAP;

    out[0] = (uint8_t)(seq & 0xFF);
    out[1] = (uint8_t)(seq >> 8);
    out[2] = (uint8_t)(n & 0xFF);
    out[3] = (uint8_t)(n >> 8);
    out[4] = flags;
    out[5] = (uint8_t)((uint16_t)pcm[0] & 0xFF);
    out[6] = (uint8_t)((uint16_t)pcm[0] >> 8);
    out[7] = *index_io;
    out[8] = tag;
    for (size_t i = 9; i < len; i++) out[i] = 0;   // the zero pad nibbles of a short block come from here

    int pred = pcm[0], index = *index_io;
    for (unsigned i = 1; i < n; i++) {
        int step = STEP[index];
        int diff = (int)pcm[i] - pred;
        unsigned code = 0;
        if (diff < 0) { code = 8; diff = -diff; }
        if (diff >= step) { code |= 4; diff -= step; }
        step >>= 1;
        if (diff >= step) { code |= 2; diff -= step; }
        step >>= 1;
        if (diff >= step) code |= 1;
        advance(&pred, &index, code);
        out[9 + (i - 1) / 2] |= (uint8_t)(code << (((i - 1) & 1) * 4));   // low nibble first
    }
    *index_io = (uint8_t)index;
    return (int)len;
}

int adpcm_decode_block(const uint8_t *payload, size_t len, int16_t *pcm, size_t pcm_cap, adpcm_info_t *info)
{
    if (!payload || len < ADPCM_PREFIX_BYTES) return ADPCM_ERR_SHORT;
    const unsigned n = (unsigned)payload[2] | (unsigned)payload[3] << 8;
    if (n < 1 || n > ADPCM_BLOCK_SAMPLES) return ADPCM_ERR_COUNT;
    if (len != adpcm_payload_len(n)) return ADPCM_ERR_LENGTH;   // from here on every index below is inside `len`
    if (payload[7] > ADPCM_INDEX_MAX) return ADPCM_ERR_INDEX;
    if (!pcm || pcm_cap < n) return ADPCM_ERR_CAP;

    int pred = (int16_t)((uint16_t)payload[5] | (uint16_t)payload[6] << 8);
    int index = payload[7];
    pcm[0] = (int16_t)pred;
    for (unsigned i = 1; i < n; i++) {
        const unsigned code = (payload[9 + (i - 1) / 2] >> (((i - 1) & 1) * 4)) & 15u;
        advance(&pred, &index, code);
        pcm[i] = (int16_t)pred;
    }
    if (info) {
        info->seq = (uint16_t)((unsigned)payload[0] | (unsigned)payload[1] << 8);
        info->n = (uint16_t)n;
        info->flags = payload[4];
        info->tag = payload[8];
        info->index_start = payload[7];
        info->index_end = (uint8_t)index;
    }
    return (int)n;
}

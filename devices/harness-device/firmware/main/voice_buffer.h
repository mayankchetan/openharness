#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

// One producer, one consumer. Publish bytes only after copying; release space only after sending.
//
// Any capacity from 1 to 2^30 B. A power of two keeps two free-running 32-bit counters (span == 0). Any other capacity (the 96 KB Wi-Fi ring of
// task W12 is 3 x 2^15) keeps the two counters as offsets in [0, span) with span = 2 x capacity: 2 x capacity is the least span in which "empty"
// (head == tail) and "full" (head == tail + capacity) differ, and a counter that wraps at span still addresses the storage by a single subtraction.
// Each side writes only its own counter, as before.
typedef struct {
    uint8_t *data;
    uint32_t capacity;
    uint32_t span;   // 0: free-running counters (power-of-two capacity); else 2 x capacity
    atomic_uint head, tail;
} voice_buffer_t;
void voice_buffer_init(voice_buffer_t *b, uint8_t *storage, uint32_t capacity);
bool voice_buffer_write(voice_buffer_t *b, const uint8_t *data, size_t size);
size_t voice_buffer_peek(voice_buffer_t *b, const uint8_t **data, size_t limit);
void voice_buffer_consume(voice_buffer_t *b, size_t size);
uint32_t voice_buffer_used(voice_buffer_t *b);

typedef struct {
    uint32_t loud_samples;
    bool heard;
} voice_gate_t;
void voice_gate_feed(voice_gate_t *gate, const int16_t *pcm, size_t samples, unsigned rate);

// Visual mic envelope only. Never used to stop capture, gate speech or modify PCM.
typedef struct { uint32_t envelope; } voice_level_t;
unsigned voice_level_feed(voice_level_t *meter, const int16_t *pcm, size_t samples);

#include "../main/voice_buffer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    voice_buffer_t b;
    uint8_t storage[1024], input[400];
    voice_buffer_init(&b, storage, sizeof(storage));
    uint32_t produced = 0, consumed = 0;
    for (int i = 0; i < 20000; i++) {
        size_t size = (unsigned)(i * 29) % sizeof(input) + 1;
        for (size_t j = 0; j < size; j++)
            input[j] = (uint8_t)(produced + j);
        if (voice_buffer_write(&b, input, size))
            produced += (uint32_t)size;
        const uint8_t *p;
        size_t n = voice_buffer_peek(&b, &p, (unsigned)(i * 13) % 300 + 1);
        for (size_t j = 0; j < n; j++)
            assert(p[j] == (uint8_t)(consumed + j));
        voice_buffer_consume(&b, n);
        consumed += (uint32_t)n;
        assert(voice_buffer_used(&b) == produced - consumed);
    }
    // Unsigned counters also wrap safely after long-running use.
    voice_buffer_init(&b, storage, sizeof(storage));
    atomic_store(&b.head, UINT32_MAX - 255);
    atomic_store(&b.tail, UINT32_MAX - 255);
    memset(input, 0x6a, sizeof(input));
    assert(voice_buffer_write(&b, input, sizeof(input)));
    const uint8_t *p;
    size_t n = voice_buffer_peek(&b, &p, 1024);
    assert(n == 256);
    voice_buffer_consume(&b, n);
    n = voice_buffer_peek(&b, &p, 1024);
    assert(n == 144);
    for (size_t j = 0; j < n; j++)
        assert(p[j] == 0x6a);
    voice_buffer_consume(&b, n);
    assert(!voice_buffer_used(&b));
    // Non-power-of-two capacity (W12). The Wi-Fi ring is 96 KB = 98,304 B = 3 x 2^15: 3.07 s of 16 kHz PCM, so a 1.5 s stall (48,000 B) fits and a
    // 4 s stall (128,000 B) does not. The counters are then offsets in [0, span) with span = 2 x capacity, not free-running 32-bit values.
    {
        static uint8_t ring[98304 + 16];
        static const uint32_t caps[] = {1000, 32768, 98304, 98303, 3};
        for (unsigned c = 0; c < sizeof caps / sizeof *caps; c++) {
            const uint32_t cap = caps[c];
            const bool pow2 = !(cap & (cap - 1));
            // every start offset that matters: 0, one before the end of the storage, the second lap, one before span
            const uint32_t starts[] = {0, cap - 1, cap, cap + 1, 2 * cap - 1};
            for (unsigned k = 0; k < sizeof starts / sizeof *starts; k++) {
                if (pow2 && starts[k] >= cap) continue;   // a power of two keeps free-running counters; the poke above covers their wrap
                memset(ring, 0xa5, sizeof ring);
                voice_buffer_init(&b, ring + 8, cap);
                assert(pow2 ? b.span == 0 : b.span == 2 * cap);
                atomic_store(&b.head, starts[k]);
                atomic_store(&b.tail, starts[k]);
                // exactly full, then one byte too many, then empty again
                static uint8_t fill[98304], back[98304];
                for (uint32_t i = 0; i < cap; i++) fill[i] = (uint8_t)(i * 7 + k);
                assert(voice_buffer_write(&b, fill, cap));
                assert(voice_buffer_used(&b) == cap);
                assert(!voice_buffer_write(&b, fill, 1));
                uint32_t got = 0;
                while (got < cap) {
                    const uint8_t *q;
                    size_t m = voice_buffer_peek(&b, &q, cap);
                    assert(m && m <= cap - got);
                    memcpy(back + got, q, m);
                    voice_buffer_consume(&b, m);
                    got += (uint32_t)m;
                }
                assert(!memcmp(fill, back, cap) && !voice_buffer_used(&b));
                // mixed sizes across the wrap, against a model
                uint32_t produced = 0, consumed = 0;
                for (int i = 0; i < 6000; i++) {
                    size_t size = (unsigned)(i * 29) % sizeof input + 1;
                    for (size_t j = 0; j < size; j++) input[j] = (uint8_t)(produced + j);
                    if (voice_buffer_write(&b, input, size)) produced += (uint32_t)size;
                    const uint8_t *q;
                    size_t m = voice_buffer_peek(&b, &q, (unsigned)(i * 13) % 700 + 1);
                    for (size_t j = 0; j < m; j++) assert(q[j] == (uint8_t)(consumed + j));
                    voice_buffer_consume(&b, m);
                    consumed += (uint32_t)m;
                    assert(voice_buffer_used(&b) == produced - consumed);
                }
                for (int i = 0; i < 8; i++) assert(ring[i] == 0xa5 && ring[cap + 8 + i] == 0xa5);   // no write outside the storage
            }
        }
        // The stall arithmetic the Wi-Fi ring is sized by: capture writes 320 B every 10 ms while the sender is blocked.
        uint8_t chunk[320] = {0};
        unsigned fits32 = 0, fits96 = 0;
        voice_buffer_init(&b, ring + 8, 32768);
        while (voice_buffer_write(&b, chunk, sizeof chunk)) fits32++;
        voice_buffer_init(&b, ring + 8, 98304);
        while (voice_buffer_write(&b, chunk, sizeof chunk)) fits96++;
        assert(fits32 == 102 && fits96 == 307);   // 1.02 s and 3.07 s
        assert(fits32 < 150 && fits96 >= 150);    // a 1.5 s stall (150 chunks) overflows the USB ring and fits the Wi-Fi ring
        assert(fits96 < 400);                     // a 4 s stall (400 chunks) overflows the Wi-Fi ring: the turn aborts, as it does today
    }
    int16_t pcm[640];
    for (int i = 0; i < 640; i++)
        pcm[i] = 2000;
    for (int chunk = 160; chunk <= 640; chunk *= 2) {
        voice_gate_t gate = {0};
        int elapsed = 0;
        while (elapsed < 5120) {
            assert(!gate.heard);
            voice_gate_feed(&gate, pcm, chunk, 16000);
            elapsed += chunk;
        }
        assert(gate.heard);
    }
    voice_gate_t gate = {0};
    voice_gate_feed(&gate, pcm, 640, 16000);
    memset(pcm, 0, sizeof(pcm));
    voice_gate_feed(&gate, pcm, 640, 16000);
    assert(!gate.heard && !gate.loud_samples);
    voice_level_t meter={0};
    assert(voice_level_feed(&meter,pcm,0)==0);
    for(int i=0;i<640;i++) pcm[i]=1500; // DC offset is not a mic signal
    assert(voice_level_feed(&meter,pcm,640)==0);
    pcm[0]=-32768; pcm[1]=32767;
    assert(voice_level_feed(&meter,pcm,640)==4);
    assert(pcm[0]==-32768 && pcm[1]==32767); // meter never modifies captured audio
    memset(pcm,0,sizeof(pcm));
    assert(voice_level_feed(&meter,pcm,160)==4); // fast attack, slow release
    for(int i=0;i<300;i++) voice_level_feed(&meter,pcm,160);
    assert(voice_level_feed(&meter,pcm,160)==0);
    pcm[0]=-150; pcm[1]=150;
    assert(voice_level_feed(&meter,pcm,160)==2); // quiet input remains visible
    puts("voice_buffer: byte order, backpressure, wraparound, non-power-of-two rings, overflow refusal and 320ms gate "
         "passed");
}

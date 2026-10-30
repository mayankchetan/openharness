#pragma once
#include "cable_client.h"
#ifdef DEVICE_PERF_BENCH
bool audio_stream_pcm(const uint8_t *pcm, size_t len);
bool audio_stream_adpcm(const uint8_t *payload, size_t len);
bool audio_stream_is_adpcm(void);
uint8_t audio_stream_voice_tag(void);
bool audio_stream_begin(const char *id, const char *cmd, const char *lang, int rate);
void audio_stream_end(void);
void audio_stream_abort(const char *reason);
void audio_probe_read(uint32_t microseconds, int bytes);
void audio_probe_run(void);
#else
#define audio_stream_pcm cable_client_voice_pcm
#define audio_stream_adpcm cable_client_voice_adpcm
#define audio_stream_is_adpcm cable_client_voice_is_adpcm
#define audio_stream_voice_tag cable_client_voice_tag
#define audio_stream_begin cable_client_voice_begin
#define audio_stream_end cable_client_voice_end
#define audio_stream_abort cable_client_voice_abort
static inline void audio_probe_read(uint32_t microseconds, int bytes)
{
    (void)microseconds;
    (void)bytes;
}
#endif

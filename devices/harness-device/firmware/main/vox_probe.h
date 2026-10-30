// VOX probe (task W4): 100 automatic voice utterances over the real voice path. Compiled only with -DDEVICE_VOX_PROBE=1 (see
// main/CMakeLists.txt); never part of a daily image.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Start the probe task (called once from app_main, after the cable client started).
void vox_start(void);
// Called by send_audio (audio_client_habitat.c) once an utterance's voice.end / voice.abort was sent. `adpcm`, `blocks` and
// `encode_us_max` describe the turn's codec: false, 0, 0 for a PCM turn.
void vox_on_sent(uint32_t produced, uint32_t delivered, uint32_t ring_peak, uint32_t read_max_us, uint32_t send_max_us,
                 bool aborted, uint32_t rx_overruns, unsigned hwm_capture, unsigned hwm_send, bool adpcm, uint32_t blocks,
                 uint32_t encode_us_max);
// Called by the cable message handler for voice.transcript / voice.error / voice.draft of the current upload.
void vox_on_reply(const char *type, const char *message);

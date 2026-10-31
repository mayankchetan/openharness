#include "audio_probe.h"
#ifdef DEVICE_PERF_BENCH
#include "audio_client.h"
#include "audio_capture.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
static atomic_bool probing;
static bool aborted;
static int64_t next_stall;
// The sink scenarios. A stall is the sender being stuck in one send: the mic goes on, the ring fills. Period 0 = once, 0.5 s into the run. The 120 ms
// stall repeats every 0.5 s (a USB host that is slow to read). The 1.5 s stall (task W12, gate G6) is the Wi-Fi case: the 96 KB ring rides it out,
// the 32 KB USB ring does not, and the line shows which ring this image has (aborted=1 means the ring overflowed).
static const struct { const char *name; uint32_t stall_ms; int64_t period_us; } scenarios[] = {
    {"healthy_sink", 0, 0},
    {"sink_stall_120ms", 120, 500000},
    {"sink_stall_1500ms", 1500, 0},
};
static uint32_t stall_ms;
static int64_t stall_period_us;
static uint32_t read_us[512], reads, produced, sent;
void audio_probe_read(uint32_t us, int bytes)
{
    if (!atomic_load(&probing))
        return;
    if (reads < 512)
        read_us[reads++] = us;
    if (bytes > 0)
        produced += (uint32_t)bytes;
}
bool audio_stream_begin(const char *id, const char *cmd, const char *lang, int rate)
{
    if (!atomic_load(&probing))
        return cable_client_voice_begin(id, cmd, lang, rate);
    return true;
}
bool audio_stream_pcm(const uint8_t *pcm, size_t size)
{
    if (!atomic_load(&probing))
        return cable_client_voice_pcm(pcm, size);
    if (stall_ms && esp_timer_get_time() >= next_stall) {
        next_stall = stall_period_us ? next_stall + stall_period_us : INT64_MAX;
        vTaskDelay(pdMS_TO_TICKS(stall_ms));
    }
    sent += (uint32_t)size;
    return true;
}
#if CABLE_VOICE_ADPCM_BUILT
// The bench probe sends nothing while probing, so a probe turn is never an ADPCM turn; outside a probe the real calls.
bool audio_stream_adpcm(const uint8_t *payload, size_t size)
{
    if (!atomic_load(&probing))
        return cable_client_voice_adpcm(payload, size);
    sent += (uint32_t)size;
    return true;
}
bool audio_stream_is_adpcm(void)
{
    return !atomic_load(&probing) && cable_client_voice_is_adpcm();
}
uint8_t audio_stream_voice_tag(void)
{
    return cable_client_voice_tag();
}
#endif
void audio_stream_end(void)
{
    if (!atomic_load(&probing))
        cable_client_voice_end();
}
void audio_stream_abort(const char *reason)
{
    if (atomic_load(&probing))
        aborted = true;
    else
        cable_client_voice_abort(reason);
}
static int compare(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
void audio_probe_run(void)
{
    // Uses the real codec, capture worker and buffer. PCM is discarded by the local sink;
    // no audio is written to a file, USB, host daemon or transcription service.
    for (int scenario = 0; scenario < 3; scenario++) {
        reads = produced = sent = 0;
        aborted = false;
        stall_ms = scenarios[scenario].stall_ms;
        stall_period_us = scenarios[scenario].period_us;
        next_stall = esp_timer_get_time() + 500000;
        atomic_store(&probing, true);
        audio_client_start_cable(NULL, VOICE_CMD_NONE);
        int64_t until = esp_timer_get_time() + 2000000;
        while (esp_timer_get_time() < until && audio_client_active())
            vTaskDelay(pdMS_TO_TICKS(10));
        audio_client_stop();
        until = esp_timer_get_time() + 3000000;
        while (audio_client_active() && esp_timer_get_time() < until)
            vTaskDelay(pdMS_TO_TICKS(10));
        if (audio_client_active()) {
            ESP_LOGE("AUDIO_PERF", "capture did not stop");
            audio_client_abort();
            return;
        }
        atomic_store(&probing, false);
        qsort(read_us, reads, sizeof(*read_us), compare);
        ESP_LOGI("AUDIO_PERF",
                 "case=%s reads=%lu read_p50_us=%lu read_p95_us=%lu produced=%lu delivered=%lu "
                 "rx_overruns=%lu aborted=%d",
                 scenarios[scenario].name, (unsigned long)reads,
                 (unsigned long)(reads ? read_us[reads / 2] : 0),
                 (unsigned long)(reads ? read_us[reads * 95 / 100] : 0), (unsigned long)produced,
                 (unsigned long)sent, (unsigned long)audio_capture_overruns(), aborted);
    }
}
#endif

// VOX probe (task W4): 100 automatic voice utterances over the real voice path. Compiled only with -DDEVICE_VOX_PROBE=1.
//
// 30 s after the cable session is up, every 6 s (start to start, or later when the reply is slow) the task does what a tap on the dial
// does: audio_client_start_cable() -> 4 s of capture -> audio_client_stop(). The capture, the ring, voice.begin, the PCM or ADPCM frames,
// voice.end and the daemon's reply are the real ones. The log line names the codec of each turn (`codec=` pcm16 or adpcm) and, for
// ADPCM, the block count and the slowest encode of the turn (`encode_us_max`, microseconds around adpcm_encode_block); both are 0 for PCM.
//
// -DDEVICE_VOX_REVIEW=1: finish with a review request (the "draft" path): the daemon makes a draft instead of typing the words
// into the agent. Needs an agent id and CABLE_FEATURE_DRAFT. The reply is then voice.draft. vox_gate.h holds the two safety gates.
//
// The words go to the first agent of the window's active tab. Put the dial on a throwaway agent tab.
#include "vox_probe.h"
#include "vox_gate.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "audio_client.h"
#include "audio_capture.h"
#include "cable_client.h"
#include "cable_features.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef DEVICE_VOX_REVIEW
#define VOX_REVIEW 1
#else
#define VOX_REVIEW 0
#endif
#define VOX_N 100
#define VOX_SETTLE_MS 30000
#define VOX_PERIOD_MS 6000
#define VOX_CAPTURE_MS 4000
#define VOX_SENT_WAIT_MS 20000
#define VOX_REPLY_WAIT_MS 40000
#define VOX_MAX_TIMEOUTS_IN_ROW 10

static const char *TAG = "vox";
static SemaphoreHandle_t s_sent_sem, s_reply_sem;
static volatile int64_t s_t_sent, s_t_reply;
static volatile bool s_got_reply, s_reply_is_error;
static char s_reply_type[16], s_reply_msg[72];
static struct {
    uint32_t produced, delivered, ring_peak, read_max_us, send_max_us, rx_overruns, blocks, encode_us_max;
    unsigned hwm_capture, hwm_send;
    bool aborted, adpcm;
} s_sent;

void vox_on_sent(uint32_t produced, uint32_t delivered, uint32_t ring_peak, uint32_t read_max_us, uint32_t send_max_us,
                 bool aborted, uint32_t rx_overruns, unsigned hwm_capture, unsigned hwm_send, bool adpcm, uint32_t blocks,
                 uint32_t encode_us_max)
{
    s_sent.produced = produced; s_sent.delivered = delivered; s_sent.ring_peak = ring_peak;
    s_sent.read_max_us = read_max_us; s_sent.send_max_us = send_max_us; s_sent.aborted = aborted;
    s_sent.rx_overruns = rx_overruns; s_sent.hwm_capture = hwm_capture; s_sent.hwm_send = hwm_send;
    s_sent.adpcm = adpcm; s_sent.blocks = blocks; s_sent.encode_us_max = encode_us_max;
    s_t_sent = esp_timer_get_time();
    if (s_sent_sem) xSemaphoreGive(s_sent_sem);
}

void vox_on_reply(const char *type, const char *message)
{
    if (s_got_reply || !s_reply_sem) return;   // the first reply of the utterance only
    s_t_reply = esp_timer_get_time();
    snprintf(s_reply_type, sizeof s_reply_type, "%s", type ? type : "");
    s_reply_is_error = type && !strcmp(type, "voice.error");
    snprintf(s_reply_msg, sizeof s_reply_msg, "%s", s_reply_is_error && message ? message : "");
    s_got_reply = true;
    xSemaphoreGive(s_reply_sem);
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static void summary(const char *tag, int n, int ok, int err, int tmo, int skipped, const int *lat, int nlat, unsigned int_min,
                    unsigned hwm_task, unsigned codec_adpcm_turns, uint32_t encode_us_max_all)
{
    int tmp[VOX_N];
    memcpy(tmp, lat, sizeof(int) * (size_t)nlat);
    qsort(tmp, (size_t)nlat, sizeof(int), cmp_int);
    int p50 = nlat ? tmp[(nlat * 50 + 99) / 100 - 1] : -1, p95 = nlat ? tmp[(nlat * 95 + 99) / 100 - 1] : -1;
    ESP_LOGI(TAG, "VOX %s n=%d replies_ok=%d replies_error=%d reply_timeouts=%d skipped=%d t_transcript_ms(min/p50/p95/max)=%d/%d/%d/%d "
             "int_free=%u int_min=%u hwm_vox_task=%u adpcm_turns=%u encode_us_max=%u", tag, n, ok, err, tmo, skipped, nlat ? tmp[0] : -1,
             p50, p95, nlat ? tmp[nlat - 1] : -1, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), int_min, hwm_task,
             codec_adpcm_turns, (unsigned)encode_us_max_all);
}

static void vox_task(void *arg)
{
    (void)arg;
    // The session must be up, and stay up, for 30 s.
    int64_t up_since = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (cable_client_is_connected()) {
            if (!up_since) up_since = now;
            if (now - up_since >= (int64_t)VOX_SETTLE_MS * 1000) break;
        } else
            up_since = 0;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    // Target: the first agent of the window's tab (what a tap on a tile uses); NULL = spoken from the overview.
    char agent[ID_MAX] = "";
    project_t *list = heap_caps_malloc(sizeof(project_t) * CABLE_MAX_AGENTS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int na = list ? cable_client_list_agents(list, CABLE_MAX_AGENTS) : 0;
    if (na > 0) snprintf(agent, sizeof agent, "%s", list[0].id);
    char name[CABLE_NAME_MAX] = "";
    if (na > 0) snprintf(name, sizeof name, "%.39s", list[0].name);
    free(list);
    const bool draft_feature = cable_client_supports(CABLE_FEATURE_DRAFT);
    // Review build: stop here, before any audio, unless the daemon can make a draft. Never fall back to sending.
    const char *start_why = vox_start_gate(VOX_REVIEW != 0, agent[0] != 0, draft_feature);
    if (start_why) {
        ESP_LOGE(TAG, "VOX ABORT reason=%s review_requested=%d agent=%d draft_feature=%d review=0 (no utterance was started)",
                 start_why, VOX_REVIEW, agent[0] != 0, draft_feature);
        ESP_LOGI(TAG, "VOX DONE utterances=0 cum_rx_overruns=0 target=");
        vTaskDelete(NULL);
        return;
    }
    bool review = VOX_REVIEW != 0;
    // The codec this dial will use for the turns that follow: the daemon's welcome is in, so the rule gives the answer now.
#if CABLE_VOICE_ADPCM_BUILT
    const char *codec_now = cable_client_voice_adpcm_wanted() ? "adpcm" : "pcm16";
#else
    const char *codec_now = "pcm16";
#endif
    ESP_LOGI(TAG, "VOX START n=%d period_ms=%d capture_ms=%d codec=%s sr=16000 target=%s agents_on_tab=%d review=%d(requested %d) "
             "int_free=%u int_min=%u", VOX_N, VOX_PERIOD_MS, VOX_CAPTURE_MS, codec_now,
             agent[0] ? "first_agent_of_tab" : "overview_route_by_daemon",
             na, review, VOX_REVIEW, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    s_sent_sem = xSemaphoreCreateBinary();
    s_reply_sem = xSemaphoreCreateBinary();

    int lat[VOX_N], nlat = 0, ok = 0, err = 0, tmo = 0, tmo_row = 0, skipped = 0, n_done = 0;
    uint32_t cum_overruns = 0, encode_us_max_all = 0;
    unsigned adpcm_turns = 0;
    for (int n = 1; n <= VOX_N; n++) {
        int64_t t0 = esp_timer_get_time();
        // the previous utterance must be fully finished (the sender clears `active` after its log line)
        for (int i = 0; i < 100 && audio_client_active(); i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (audio_client_active()) {
            skipped++;
            ESP_LOGW(TAG, "VOX n=%d SKIP audio still active", n);
            continue;
        }
        xSemaphoreTake(s_sent_sem, 0);
        xSemaphoreTake(s_reply_sem, 0);
        s_got_reply = false; s_t_reply = 0; s_t_sent = 0;
        memset(&s_sent, 0, sizeof s_sent);
        audio_client_start_cable(agent[0] ? agent : NULL, VOICE_CMD_NONE);
        vTaskDelay(pdMS_TO_TICKS(VOX_CAPTURE_MS));
        if (review) {
            audio_client_request_review();
            if (!audio_client_review_requested()) {   // without the flag, voice.end would send the words
                audio_client_abort();
                ESP_LOGE(TAG, "VOX ABORT reason=review_not_set n=%d (utterance aborted, nothing sent)", n);
                n_done = n;
                break;
            }
        }
        int64_t t_fin = esp_timer_get_time();
        audio_client_stop();
        bool sent = xSemaphoreTake(s_sent_sem, pdMS_TO_TICKS(VOX_SENT_WAIT_MS)) == pdTRUE;
        bool reply = false;
        if (sent && !s_sent.aborted) reply = xSemaphoreTake(s_reply_sem, pdMS_TO_TICKS(VOX_REPLY_WAIT_MS)) == pdTRUE;
        int send_ms = sent ? (int)((s_t_sent - t_fin) / 1000) : -1;
        int t_tr = reply ? (int)((s_t_reply - t_fin) / 1000) : -1;
        const char *kind = !sent ? "no_sent_event" : s_sent.aborted ? "aborted" : !reply ? "reply_timeout"
                           : s_reply_is_error ? "error" : s_reply_type[6] ? s_reply_type + 6 : "reply";
        if (reply) {
            lat[nlat++] = t_tr;
            if (s_reply_is_error) err++; else ok++;
            tmo_row = 0;
        } else if (sent && !s_sent.aborted) {
            tmo++; tmo_row++;
        }
        cum_overruns += s_sent.rx_overruns;
        if (s_sent.adpcm) adpcm_turns++;
        if (s_sent.encode_us_max > encode_us_max_all) encode_us_max_all = s_sent.encode_us_max;
        n_done = n;
        ESP_LOGI(TAG, "VOX n=%d cap_frames=%u overruns=%u dropped=%u sent_bytes=%u codec=%s blocks=%u encode_us_max=%u send_ms=%d "
                 "t_transcript_ms=%d kind=%s int_free=%u int_min=%u hwm_capture=%u hwm_send=%u hwm_vox=%u ring_peak=%u read_max_us=%u "
                 "send_max_us=%u aborted=%d%s%s%s",
                 n, (unsigned)(s_sent.produced / 320), (unsigned)s_sent.rx_overruns, (unsigned)(s_sent.produced - s_sent.delivered),
                 (unsigned)s_sent.delivered, s_sent.adpcm ? "adpcm" : "pcm16", (unsigned)s_sent.blocks, (unsigned)s_sent.encode_us_max,
                 send_ms, t_tr, kind, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), s_sent.hwm_capture, s_sent.hwm_send,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)s_sent.ring_peak, (unsigned)s_sent.read_max_us,
                 (unsigned)s_sent.send_max_us, s_sent.aborted, s_reply_is_error ? " msg=\"" : "", s_reply_is_error ? s_reply_msg : "",
                 s_reply_is_error ? "\"" : "");
        if (n % 10 == 0)
            summary("SUMMARY", n, ok, err, tmo, skipped, lat, nlat, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)uxTaskGetStackHighWaterMark(NULL), adpcm_turns, encode_us_max_all);
        const char *reply_why = reply ? vox_reply_gate(VOX_REVIEW != 0, s_reply_type) : NULL;
        if (reply_why) {
            ESP_LOGE(TAG, "VOX ABORT reason=%s n=%d reply=%s (a review run got a reply that is not a draft; the words may have been sent)",
                     reply_why, n, s_reply_type);
            break;
        }
        if (tmo_row >= VOX_MAX_TIMEOUTS_IN_ROW) {
            ESP_LOGE(TAG, "VOX ABORT %d reply timeouts in a row (daemon not answering)", tmo_row);
            break;
        }
        int64_t wait_ms = (int64_t)VOX_PERIOD_MS - (esp_timer_get_time() - t0) / 1000;
        if (wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
    ESP_LOGI(TAG, "VOX DONE utterances=%d cum_rx_overruns=%u adpcm_turns=%u encode_us_max=%u target=%s", n_done, (unsigned)cum_overruns,
             adpcm_turns, (unsigned)encode_us_max_all, name);
    vTaskDelete(NULL);
}

void vox_start(void)
{
    xTaskCreatePinnedToCore(vox_task, "vox", 6144, NULL, 2, NULL, 1);
}

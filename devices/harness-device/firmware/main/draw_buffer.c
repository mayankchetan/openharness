#include "draw_buffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"   // esp_cache_get_alignment(): private in IDF 5.5.6, hence PRIV_REQUIRES esp_mm

static const char *TAG = "draw_buffer";

void *draw_buffer_alloc(size_t bytes, bool want_psram, bool *in_psram)
{
    if (in_psram) *in_psram = false;
    if (want_psram) {
        size_t line = 0;
        if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &line) == ESP_OK && line && !(line & (line - 1))) {
            void *buffer = heap_caps_aligned_alloc(line, (bytes + line - 1) & ~(line - 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (buffer) {
                if (in_psram) *in_psram = true;
                return buffer;
            }
            ESP_LOGW(TAG, "%u B PSRAM draw buffer failed, using internal RAM", (unsigned)bytes);
        } else {
            ESP_LOGW(TAG, "PSRAM cache line size unknown (%u), using internal RAM for a %u B draw buffer", (unsigned)line, (unsigned)bytes);
        }
    }
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

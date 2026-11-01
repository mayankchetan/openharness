#include "usb_probe.h"

#include <stdlib.h>
#include <string.h>

#include "cable_client.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_probe_core.h"

static const char *TAG = "usb_probe";

// The driver needs RX > 64 and TX > 0 (usb_serial_jtag.c). RX holds a burst from an older daemon, which pushes its roster after a probe hello,
// while this task is between reads (it reads every 40 ms); TX holds a few of the 70-byte hellos if the host has not opened the port yet.
#define PROBE_RX_BUF 1024
#define PROBE_TX_BUF 256
#define PROBE_WRITE_WAIT_MS 20

static up_core_t s_core;
static uint8_t *s_probe_buf;
static bool s_running;

bool usb_probe_running(void) { return s_running; }

bool usb_probe_start(uint64_t now_ms)
{
    if (s_running) return true;
    // The decoder is a CPU-only parse buffer (no DMA): PSRAM is fine, and keeps 1 KB of internal RAM free.
    s_probe_buf = heap_caps_malloc(UP_FRAME_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_probe_buf) s_probe_buf = heap_caps_malloc(UP_FRAME_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_probe_buf || !up_core_init(&s_core, s_probe_buf, UP_FRAME_CAP)) {
        ESP_LOGW(TAG, "no memory for the probe's decoder");
        free(s_probe_buf);
        s_probe_buf = NULL;
        return false;
    }
    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = PROBE_TX_BUF, .rx_buffer_size = PROBE_RX_BUF};
    const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "usb_serial_jtag driver install failed (%s): the probe is off for now", esp_err_to_name(err));
        free(s_probe_buf);
        s_probe_buf = NULL;
        return false;
    }
    up_core_start(&s_core, now_ms);
    s_running = true;
    ESP_LOGI(TAG, "probe on (a host is driving the USB port)");
    return true;
}

void usb_probe_stop(void)
{
    if (!s_running) return;
    s_running = false;
    usb_serial_jtag_driver_uninstall();
    free(s_probe_buf);
    s_probe_buf = NULL;
    memset(&s_core, 0, sizeof s_core);
    ESP_LOGI(TAG, "probe off");
}

static int port_read(void *ctx, uint8_t *buf, size_t cap, uint32_t wait_ms)
{
    (void)ctx;
    return usb_serial_jtag_read_bytes(buf, cap, pdMS_TO_TICKS(wait_ms));
}

void usb_probe_poll(uint64_t now_ms, uint32_t wait_ms)
{
    if (!s_running) return;
    // A bounded read (up_core_pump): a host that streams frames as fast as the port takes them must not keep this task out of the idle task for
    // long enough to trip the task watchdog. After a call that hit the bound, sleep one tick; the rest is read by the next poll.
    if (up_core_pump(&s_core, port_read, NULL, wait_ms, now_ms)) vTaskDelay(1);
    if (up_core_hello_due(&s_core, now_ms)) {
        uint8_t frame[96];
        const size_t len = up_core_hello(frame, sizeof frame, CABLE_PROTO_VERSION, CABLE_PRODUCT);
        // A host that has not opened the port does not drain the ring; the write then times out, which is normal here (cable_link_send says the same).
        if (len) usb_serial_jtag_write_bytes(frame, len, pdMS_TO_TICKS(PROBE_WRITE_WAIT_MS));
    }
}

bool usb_probe_last_welcome(uint64_t *at_ms)
{
    if (!s_running || !s_core.have_welcome) return false;
    if (at_ms) *at_ms = s_core.last_welcome_ms;
    return true;
}

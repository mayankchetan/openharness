// The USB-Serial-JTAG transport: the only code that touches the native USB port's driver.
// Moved out of cable_link.c unchanged (task W5); cable_link.c is now wire-agnostic. See cable_xport.h.
#include "cable_xport.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "cable";

// Driver FIFOs.
//
// ⚠️ RX IS NOT A CONVENIENCE — IT IS THE ONLY THING BETWEEN A SLOW READER AND SILENT DATA LOSS. There is
// no back-pressure on this peripheral. The IDF driver's ISR (esp_driver_usb_serial_jtag/src/
// usb_serial_jtag.c) does:
//
//     rx_fifo_len = usb_serial_jtag_ll_read_rxfifo(buf, USB_SER_JTAG_RX_MAX_SIZE);
//     xRingbufferSendFromISR(rx_ring_buf, buf, rx_fifo_len, &xTaskWoken);
//
// It drains the hardware FIFO unconditionally and ignores the ringbuffer's return value, so a full ring
// means those bytes are simply gone — and because emptying the HW FIFO is what lets the peripheral ACK
// the next OUT packet, the host is never NAK'd and never learns anything went missing. Not reading fast
// enough does not slow the sender down; it shreds the stream.
//
// Measured on this hardware by the sibling firmware, not theorised: with a smaller ring a firmware
// transfer died at 0 of 1,342,160 bytes written. Each 8 KB slice takes ~16 ms to reach flash, and at USB
// full speed ~16 KB arrives while the reader task is inside that write.
//
// So the rule this file keeps: RX_BUF ≥ the largest number of bytes the peer may have in flight while
// this task is blocked. The firmware transfer is the only sender that can saturate the link, and its
// credit window is 16 KB (docs/specs/cable-protocol.md, "Firmware update"). 32 KB is that window twice
// over, for scheduling jitter. Changing either number without the other reintroduces exactly this bug.
#define USJ_RX_BUF (32 * 1024)

// TX is sized so the largest thing this link really sends — a PCM chunk plus framing — fits in one go and
// the write does not block halfway through a frame waiting for the host to drain. (CABLE_MAX_PAYLOAD is
// 8192, but that is a bound on damage from a corrupt length field, not a size anything actually sends.)
#define USJ_TX_BUF 2048

static bool usb_open(const cable_xport_t *x)
{
    (void)x;
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = USJ_TX_BUF,
        .rx_buffer_size = USJ_RX_BUF,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        // Say what it costs. The symptom lands far from here — the dial comes up, draws its screen, and
        // simply never hears from the machine — so the log line has to name the cause itself.
        ESP_LOGE(TAG, "usb_serial_jtag driver install failed (%s) — no link to the daemon",
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

static void usb_close(const cable_xport_t *x)
{
    (void)x;
    usb_serial_jtag_driver_uninstall();
}

static int usb_read(const cable_xport_t *x, uint8_t *buf, size_t cap, int wait_ms)
{
    (void)x;
    return usb_serial_jtag_read_bytes(buf, cap, pdMS_TO_TICKS(wait_ms));
}

// One call, one frame: the driver copies into its TX ring, and a count short of `len` means the host is
// not draining the port. The far decoder resyncs past a cut frame by magic scan, so a partial count is
// legal here (cable_xport.h).
static int usb_write(const cable_xport_t *x, const uint8_t *buf, size_t len, int wait_ms)
{
    (void)x;
    return usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(wait_ms));
}

static bool usb_is_up(const cable_xport_t *x)
{
    (void)x;
    return usb_serial_jtag_is_connected();
}

static const cable_xport_t s_usb = {
    .name = "usb",
    .kind = CABLE_XPORT_USB,
    .open = usb_open,
    .close = usb_close,
    .read = usb_read,
    .write = usb_write,
    .is_up = usb_is_up,
};

const cable_xport_t *cable_xport_usb(void) { return &s_usb; }

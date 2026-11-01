#include "usb_bounce.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"

#define BOUNCE_MS 100   // the value the sleep plan names (plan section 3.5, R1): a host must see the pull-up gone; short enough to sit in one render pass

void usb_dp_bounce(void)
{
    // All four pulls forced off: D+ pull-up off is the "unplugged" the host sees; D- and the pull-downs are not used by a device, they stay off.
    const usb_serial_jtag_pull_override_vals_t off = {.dp_pu = false, .dm_pu = false, .dp_pd = false, .dm_pd = false};
    usb_serial_jtag_ll_phy_enable_pull_override(&off);
    vTaskDelay(pdMS_TO_TICKS(BOUNCE_MS));
    usb_serial_jtag_ll_phy_disable_pull_override();   // the PHY's own pull-up is back: the host enumerates afresh
}

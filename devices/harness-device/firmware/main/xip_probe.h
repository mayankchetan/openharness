#pragma once

// XiP probe (DEVICE_XIP_PROBE builds only; see xip_probe.c). Starts the controller task; call once, late in
// app_main. Log prefix "XIP", end marker "XIP DONE".
void xip_probe_start(void);

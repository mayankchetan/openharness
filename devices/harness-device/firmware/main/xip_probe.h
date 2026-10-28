#pragma once

// XiP probe (DEVICE_XIP_PROBE builds only; see xip_probe.c). Starts the controller task; call once, late in
// app_main. Log prefix "XIP", end marker "XIP DONE".
void xip_probe_start(void);
// Loud banner "XIP PROBE BUILD - NOT FOR PRODUCTION" (several lines). Called first in app_main(), again at the
// start of the probe and again at the end ("XIP DONE"), so a probe image cannot be mistaken for production.
void xip_probe_banner(void);

#pragma once
// The RTC watchdog for the time the chip may light-sleep (battery sleep only; sleep_wdt.c). The task watchdog and the interrupt watchdog run on a timer
// group whose clock stops in light sleep, so they count awake time only: the hang of 2026-11-01 on dial :50 was found after 62 hours. The RTC watchdog
// runs in the RTC domain through light sleep. power_sleep.c arms it when it releases the awake lock for S2, feeds it from every ptt wake (250 ms in
// S2) and disarms it when S2 ends. If the chip never wakes, or the ptt task stops, the system resets within `ms` (reset reason: WATCHDOG, so the last
// words and the breadcrumb are reported). The stage action is a system reset, which keeps RTC memory (the ring and the breadcrumb).
#include <stdint.h>

void sleep_wdt_arm(uint32_t ms);
void sleep_wdt_feed(void);
void sleep_wdt_disarm(void);

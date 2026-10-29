#pragma once
// THE STORE GATE (DEVICE_BATTERY_POWEROFF): every NVS writer of this firmware (config_store.c, battery_log.c) brackets its write with
// store_gate_enter() ... store_gate_leave(). When the PMIC is about to switch the dial off (power_off.c), store_gate_freeze() stops new
// writers and waits for the ones in flight, so the power is never cut in the middle of a flash write (plan 16.1 item 2). A refused enter
// is a write that does not happen. Lock-free: two atomics. Without the flag the calls are empty inlines and the writers are as they were.
#include <stdbool.h>
#include <stdint.h>

#ifdef DEVICE_BATTERY_POWEROFF
// False while frozen: do not write. True: write, then call store_gate_leave() on every path.
bool store_gate_enter(void);
void store_gate_leave(void);
// Stop new writers and wait up to timeout_ms for the ones in flight. True: no writer is running and none can start. False: one is still
// running after the timeout; the gate is open again.
bool store_gate_freeze(uint32_t timeout_ms);
void store_gate_thaw(void);
bool store_gate_frozen(void);
#else
static inline bool store_gate_enter(void) { return true; }
static inline void store_gate_leave(void) {}
#endif

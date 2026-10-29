#include "store_gate.h"

#ifdef DEVICE_BATTERY_POWEROFF

#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static atomic_uint s_writers;     // writers between enter and leave
static atomic_bool s_frozen;

bool store_gate_enter(void)
{
    if (atomic_load(&s_frozen)) return false;
    atomic_fetch_add(&s_writers, 1);
    if (atomic_load(&s_frozen)) {      // the freeze began between the two lines: give the slot back, as a refusal
        atomic_fetch_sub(&s_writers, 1);
        return false;
    }
    return true;
}

void store_gate_leave(void) { atomic_fetch_sub(&s_writers, 1); }

bool store_gate_freeze(uint32_t timeout_ms)
{
    atomic_store(&s_frozen, true);     // first: from here no writer can start
    for (uint32_t waited = 0;; waited += 10) {
        if (atomic_load(&s_writers) == 0) return true;
        if (waited >= timeout_ms) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    atomic_store(&s_frozen, false);
    return false;
}

void store_gate_thaw(void) { atomic_store(&s_frozen, false); }

bool store_gate_frozen(void) { return atomic_load(&s_frozen); }

#endif

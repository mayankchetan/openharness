#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Create a task whose STACK is in PSRAM (the task control block stays internal, as FreeRTOS requires). If the
// PSRAM stack cannot be allocated, log a warning and create the same task on an internal stack instead, so a
// full PSRAM never costs a task. Arguments are those of xTaskCreatePinnedToCore() (stack_bytes in bytes, core
// may be tskNO_AFFINITY, out may be NULL). Returns pdPASS or the failure code of the internal create.
//
// Only for tasks that run forever. A task made this way must not call vTaskDelete(NULL) (that leaks the PSRAM
// stack; use vTaskDeleteWithCaps), and must not reach esp_ota_*, esp_partition_mmap or any other flash-mapping
// call: those take the cache-freeze path, which asserts when the stack is in PSRAM (diet2-r1-build.md section 11).
// Each caller has a TASK_STACK_PSRAM_* macro; 0 puts it back on xTaskCreate*.
BaseType_t task_create_psram(TaskFunction_t fn, const char *name, uint32_t stack_bytes, void *arg,
                             UBaseType_t priority, TaskHandle_t *out, BaseType_t core);

// Per-task switches (compile with -DTASK_STACK_PSRAM_<X>=0 to put that task's stack back in internal RAM).
// Stack bytes moved out of internal RAM at the default: ptt 4,096, beep 4,096.
#ifndef TASK_STACK_PSRAM_PTT
#define TASK_STACK_PSRAM_PTT 1
#endif
#ifndef TASK_STACK_PSRAM_BEEP
#define TASK_STACK_PSRAM_BEEP 1
#endif

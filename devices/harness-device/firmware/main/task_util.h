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
// Stack bytes moved out of internal RAM at the default: ptt 4,096, beep 4,096, proj_refresh 7,168; FLASH_OPS
// (habitat_actions 6,144, cable_session 4,096, voice_send 5,120) 15,360; net_link 8,192 (Wi-Fi mode only).
#ifndef TASK_STACK_PSRAM_PTT
#define TASK_STACK_PSRAM_PTT 1
#endif
#ifndef TASK_STACK_PSRAM_BEEP
#define TASK_STACK_PSRAM_BEEP 1
#endif
#ifndef TASK_STACK_PSRAM_PROJ
#define TASK_STACK_PSRAM_PROJ 1
#endif
// The three tasks that save to NVS or read config but never map flash: habitat_actions (config_save_*), cable_session
// (send_hello, config_load_voicelang) and voice_send (config_load_voicelang). NVS writes from a PSRAM stack passed the
// XiP probe (1,000 write+commit on each core, and against esp_ota_end; diet2-r1-results.md section 1). cable_link is NOT
// in this group: with OTA on, its reader runs fw_update_* -> esp_ota_*, and esp_ota_begin asserts on a PSRAM stack.
#ifndef TASK_STACK_PSRAM_FLASH_OPS
#define TASK_STACK_PSRAM_FLASH_OPS 1
#endif
// net_link (Wi-Fi mode, 8,192 B): connect, TLS handshake, sockets, backoff. Its call graph holds no flash operation (net_link.h states it and
// test_net_link_source.py checks it); the NVS and PHY work of the Wi-Fi bring-up runs on the short-lived internal-stack net_init task.
#ifndef TASK_STACK_PSRAM_NET
#define TASK_STACK_PSRAM_NET 1
#endif
// The cable_link reader (6,144 B). Its only flash user was the OTA path, so once that is compiled out (DEVICE_NO_OTA, plan
// C17) it is a task like the three above: it reads the USB port and parses frames; the one NVS call it reaches is a read
// (config_load_voicelang via ui_settings_read). The Wi-Fi settings' writes (net.set and its kin) are handed to the net_store task
// with an internal stack (cable_client.c handle_net), as power_off_request does. Never on with OTA: cable_link.c turns that into a
// compile error.
#ifndef TASK_STACK_PSRAM_READER
#ifdef DEVICE_NO_OTA
#define TASK_STACK_PSRAM_READER 1
#else
#define TASK_STACK_PSRAM_READER 0
#endif
#endif
#if TASK_STACK_PSRAM_READER && !defined(DEVICE_NO_OTA)
#error "TASK_STACK_PSRAM_READER needs DEVICE_NO_OTA: the OTA path (esp_ota_*) asserts on a PSRAM stack"
#endif

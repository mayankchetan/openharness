// Host stand-in for the few FreeRTOS/IDF names task_util.c uses (test_task_util.c compiles it on the PC).
#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdPASS 1
#define errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY (-1)
#define tskNO_AFFINITY 0x7FFFFFFF
#define MALLOC_CAP_SPIRAM (1u << 10)
// critical sections and the stack mark (uim.c)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))

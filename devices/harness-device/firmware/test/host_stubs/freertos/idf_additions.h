#pragma once
#include "FreeRTOS.h"
void vTaskDeleteWithCaps(TaskHandle_t task);
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core, UBaseType_t caps);

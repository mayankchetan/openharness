#include "task_util.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"   // xTaskCreatePinnedToCoreWithCaps

static const char *TAG = "task_util";

void task_delete_psram(TaskHandle_t task)
{
    if (!task) return;
    StackType_t *stack = NULL;
    StaticTask_t *tcb = NULL;
    if (xTaskGetStaticBuffers(task, &stack, &tcb) == pdTRUE) vTaskDeleteWithCaps(task);   // made with caps: the PSRAM stack and the control block go back
    else vTaskDelete(task);
}

BaseType_t task_create_psram(TaskFunction_t fn, const char *name, uint32_t stack_bytes, void *arg,
                             UBaseType_t priority, TaskHandle_t *out, BaseType_t core)
{
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, arg, priority, out, core, MALLOC_CAP_SPIRAM);
    if (rc == pdPASS) return rc;
    ESP_LOGW(TAG, "%s: %u B PSRAM stack failed (rc=%d), using an internal stack", name, (unsigned)stack_bytes, (int)rc);
    return xTaskCreatePinnedToCore(fn, name, stack_bytes, arg, priority, out, core);
}

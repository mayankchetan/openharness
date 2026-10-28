// task_create_psram(): a PSRAM stack when it can be had, the old internal stack when it cannot. The two
// IDF create calls are fakes here; the test proves which one runs, with which arguments, and what comes back.
#include "task_util.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int caps_calls, internal_calls, caps_result, internal_result;
static struct { TaskFunction_t fn; const char *name; uint32_t stack; void *arg; UBaseType_t prio;
                TaskHandle_t *out; BaseType_t core; UBaseType_t caps; } last_caps, last_internal;
static TaskHandle_t handle_given;

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core, UBaseType_t caps)
{
    caps_calls++;
    last_caps.fn = fn; last_caps.name = name; last_caps.stack = stack; last_caps.arg = arg;
    last_caps.prio = prio; last_caps.out = out; last_caps.core = core; last_caps.caps = caps;
    if (caps_result == pdPASS && out) *out = handle_given;
    return caps_result;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    internal_calls++;
    last_internal.fn = fn; last_internal.name = name; last_internal.stack = stack; last_internal.arg = arg;
    last_internal.prio = prio; last_internal.out = out; last_internal.core = core;
    if (internal_result == pdPASS && out) *out = handle_given;
    return internal_result;
}

static int warnings;
void host_log_warn(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; warnings++; }

static void body(void *arg) { (void)arg; }

static void reset(int caps_rc, int internal_rc)
{
    caps_calls = internal_calls = warnings = 0;
    caps_result = caps_rc; internal_result = internal_rc;
    memset(&last_caps, 0, sizeof last_caps); memset(&last_internal, 0, sizeof last_internal);
}

int main(void)
{
    int token;
    TaskHandle_t out = NULL;
    handle_given = (TaskHandle_t)&token;

    // PSRAM create works: one caps call, no internal create, every argument forwarded, MALLOC_CAP_SPIRAM.
    reset(pdPASS, pdPASS);
    assert(task_create_psram(body, "ptt", 4096, &token, 3, &out, tskNO_AFFINITY) == pdPASS);
    assert(caps_calls == 1 && internal_calls == 0 && warnings == 0);
    assert(last_caps.fn == body && !strcmp(last_caps.name, "ptt") && last_caps.stack == 4096);
    assert(last_caps.arg == &token && last_caps.prio == 3 && last_caps.out == &out);
    assert(last_caps.core == tskNO_AFFINITY && last_caps.caps == MALLOC_CAP_SPIRAM);
    assert(out == handle_given);

    // PSRAM create fails: the same task is created on an internal stack with the same arguments.
    out = NULL;
    reset(errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY, pdPASS);
    assert(task_create_psram(body, "beep", 4096, NULL, 5, &out, 1) == pdPASS);
    assert(caps_calls == 1 && internal_calls == 1 && warnings == 1);
    assert(last_internal.fn == body && !strcmp(last_internal.name, "beep") && last_internal.stack == 4096);
    assert(last_internal.arg == NULL && last_internal.prio == 5 && last_internal.out == &out && last_internal.core == 1);
    assert(out == handle_given);

    // Both fail: the caller sees the failure (the internal create's code), and no handle is written.
    out = NULL;
    reset(errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY, errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
    assert(task_create_psram(body, "proj_refresh", 7168, NULL, 4, &out, tskNO_AFFINITY)
           == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
    assert(caps_calls == 1 && internal_calls == 1 && out == NULL);

    // A NULL handle pointer is allowed (ptt keeps no handle).
    reset(pdPASS, pdPASS);
    assert(task_create_psram(body, "ptt", 4096, NULL, 3, NULL, tskNO_AFFINITY) == pdPASS);
    assert(caps_calls == 1 && last_caps.out == NULL);

    puts("task_create_psram: PSRAM stack first, internal fallback with the same arguments, failure passed on PASS");
    return 0;
}

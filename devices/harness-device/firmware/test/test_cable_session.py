"""Run the real connection loops with an adversarial task-start/read schedule."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

main = Path(__file__).resolve().parent / '../main'
client = (main / 'cable_client.c').read_text()
link = (main / 'cable_link.c').read_text()

def function(source, name):
    match = re.search(r'^[^\n]*\b' + name + r'\([^;]*?\)\n\{.*?^\}', source, re.M | re.S)
    assert match, name
    return match.group(0) + '\n'

# Single-writer ownership is part of the contract, not a timing hope.
assert 'session_down(' not in function(client, 'session_task')
assert 's_last_rx_us' not in function(client, 'session_task')
assert 'static int64_t' in client and 'static atomic_bool' in client

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <setjmp.h>
#include "cable_xport.h"
#include "power_sleep.h"   /* the loops ask power_sleep_wait_ms / power_sleep_relaxed: their own waits, no sleeping, with the flag off */
#define READ_CHUNK 256
#define READ_WAIT_MS 100
#define FRAME_IDLE_US INT64_C(15000000)
#define SILENCE_MS 15000
#define READER_STACK 6144
#define CABLE_HEADER_BYTES 6
#define CABLE_CRC_BYTES 2
#define CABLE_FRAME_VERSION 1
#define CABLE_MAX_FRAME 8204
#define CABLE_MAX_AGENTS 4
#define CABLE_PROTO_VERSION 3
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_OK 0
#define pdPASS 1
typedef int BaseType_t;
#define pdMS_TO_TICKS(ms) (ms)
typedef int esp_err_t;
typedef void (*cable_frame_cb)(uint8_t,uint8_t,const uint8_t *,size_t,void *);
typedef void (*cable_tick_cb)(void *);
static bool connected, driver, fail_driver, fail_reader, new_task_ready, s_started;
typedef void *TaskHandle_t;
static TaskHandle_t hello_handle;
static unsigned attempts, fail_at, live, hellos, notifications;
static atomic_bool s_session, s_running;
static void *s_tx_lock, *s_agents_lock, *s_models_sem;
static int s_machines;
static int64_t now, s_last_rx_us;
static unsigned read_index, ticks, down_count, frames;
static int owner;
static jmp_buf loop_done;
static cable_frame_cb s_cb;
static cable_tick_cb s_tick;
static void *s_ctx;
typedef struct {size_t len;} cable_decoder_buf_t;
static cable_decoder_buf_t s_decoder;
static uint8_t s_dec_buf[CABLE_MAX_FRAME];
static const cable_xport_t *s_xport;
typedef int cable_agent_t;
static cable_agent_t *s_agents;
static int64_t esp_timer_get_time(void) { return now; }
static void fw_update_tick(void) {}
static void ui_set_connected(bool value) { connected = value; }
static void session_down(const char *why) {
    (void)why; assert(owner == 1); down_count++; s_session = false; connected = false;
}
'''
code += function(client, 'session_tick')
code += r'''
static void on_frame(uint8_t version,uint8_t type,const uint8_t *p,size_t n,void *ctx) {
    (void)version; (void)type; (void)p; (void)n; assert(ctx == NULL && owner == 1);
    frames++; s_last_rx_us = now; s_session = true; connected = true;
}
static bool cable_decoder_init_with(cable_decoder_buf_t *d,uint8_t *buf,size_t cap) {
    assert(buf == s_dec_buf && cap == CABLE_MAX_FRAME); d->len = 0; return true;
}
static void cable_decoder_buf_reset(cable_decoder_buf_t *d) { d->len = 0; }
static void cable_decoder_buf_feed(cable_decoder_buf_t *d,const uint8_t *p,size_t n,cable_frame_cb cb,void *ctx) {
    (void)d; assert(n == 1); cb(1,1,p,n,ctx);
}
static void counted_tick(void *ctx) { ticks++; session_tick(ctx); }
// The fake transport (the reader and the start path see only the vtable). open/close stand in for the driver
// install and uninstall, with the same allocation-failure schedule.
static int fake_read(const cable_xport_t *x,uint8_t *p,size_t n,int timeout) {
    assert(x == s_xport && n == READ_CHUNK && timeout == 100 && owner == 1);
    switch (read_index++) {
    case 0: now = (INT64_C(1) << 32) - 1; p[0] = 0; return 1;
    case 1: now += 100; return 0; // 32-bit low word rolled over, still connected.
    case 2: assert(s_session); now += INT64_C(15000001); return -1;
    case 3: assert(!s_session && down_count == 1); p[0] = 0; return 1;
    default: assert(s_session && down_count == 1); longjmp(loop_done,1);
    }
}
static bool allocation(void) { return ++attempts != fail_at; }
static void *tracked_calloc(size_t n,size_t size) {
    if(!allocation())return NULL;
    void *p=calloc(n,size);assert(p);live++;return p;
}
static void tracked_free(void *p) { if(p){assert(live);live--;free(p);} }
// The agent list lives in PSRAM on the board (ram_psram_calloc); the host stub counts the calls and keeps the
// leak accounting of tracked_calloc.
static unsigned psram_calls;
static void *ram_psram_calloc(size_t n,size_t size,const char *owner) { (void)owner;psram_calls++;return tracked_calloc(n,size); }
static void *xSemaphoreCreateMutex(void) { return tracked_calloc(1,1); }
static void *xSemaphoreCreateBinary(void) { return tracked_calloc(1,1); }
static void vSemaphoreDelete(void *lock) { assert(lock);tracked_free(lock); }
static bool fake_open(const cable_xport_t *x) {
    assert(x == s_xport);
    driver = allocation() && !fail_driver; return driver;
}
static void fake_close(const cable_xport_t *x) { assert(x == s_xport); driver = false; }
static const cable_xport_t fake_xport = { "fake", CABLE_XPORT_USB, fake_open, fake_close, fake_read, NULL, NULL, NULL, NULL };
const cable_xport_t *cable_xport_usb(void) { return &fake_xport; }
static void cable_machines_init(int *m) { *m = 1; }
static void session_task(void *arg) { (void)arg; }
static void reader_task(void *arg);
static int xTaskCreate(void (*task)(void *),const char *name,int stack,void *arg,int priority,void *handle) {
    (void)name; (void)stack; (void)arg; (void)priority;
    if(!allocation())return 0;
    if (task != reader_task) {
        assert(task == session_task && handle && !hellos);
        hello_handle=(void *)2;*(TaskHandle_t *)handle=hello_handle;hellos++;return pdPASS;
    }
    if (fail_reader) return 0;
    assert(driver && s_running && s_tick && !connected && s_last_rx_us == now);
    // A welcome can arrive on the new reader before xTaskCreate returns.
    owner = 1; s_cb(1,1,NULL,0,s_ctx); owner = 0; new_task_ready = true;
    return pdPASS;
}
// cable_session's stack is in PSRAM on the board (TASK_STACK_PSRAM_FLASH_OPS), and so is the reader's when the OTA path is
// compiled out (TASK_STACK_PSRAM_READER); the host stub counts the call and takes the same path as xTaskCreate, so the
// failure schedule below is the same in every setting.
#define tskNO_AFFINITY (-1)
static unsigned psram_creates;
__attribute__((unused)) static int task_create_psram(void (*task)(void *),const char *name,int stack,void *arg,int priority,void *handle,int core) {
    (void)core; psram_creates++; return xTaskCreate(task,name,stack,arg,priority,handle);
}
static void vTaskDelete(TaskHandle_t task) { assert(task==hello_handle&&hellos);hellos--;hello_handle=NULL; }
static void xTaskNotifyGive(TaskHandle_t task) {
    assert(task==hello_handle&&hellos&&s_running&&driver&&new_task_ready);notifications++;
}
#define calloc tracked_calloc
#define free tracked_free
'''
code += function(link, 'reader_task')
code += function(link, 'cable_link_start_with') + function(link, 'cable_link_start')
code += function(client, 'cable_client_start')
code += r'''
int main(void) {
    // Array, both metadata locks, handshake task, transmit lock, driver, reader.
    // Every startup failure must release all resources and allow a clean retry.
    for(unsigned failure=1;failure<=7;failure++) {
        attempts=0;fail_at=failure;
        assert(!cable_client_start());
        assert(!s_agents&&!s_agents_lock&&!s_models_sem&&!s_tx_lock&&!s_started);
        assert(!s_running&&!driver&&!hellos&&!live&&!notifications);
    }
    attempts=fail_at=0;
    assert(psram_calls>=1); // s_agents comes from ram_psram_calloc, not from the internal-first calloc
    // the handshake task (TASK_STACK_PSRAM_FLASH_OPS): failure points 4-7 reach the create; the reader (TASK_STACK_PSRAM_READER): point 7
    assert(psram_creates==4*TASK_STACK_PSRAM_FLASH_OPS+TASK_STACK_PSRAM_READER);
    now = 91; assert(cable_client_start());
    assert(new_task_ready && s_running && connected && s_session && notifications==1);
    // and the clean start made its handshake task and its reader through task_create_psram too
    assert(psram_creates==5*TASK_STACK_PSRAM_FLASH_OPS+2*TASK_STACK_PSRAM_READER);
    unsigned calls=attempts;assert(cable_client_start());assert(calls==attempts&&notifications==1);
    s_tick = counted_tick; owner = 1;
    if (!setjmp(loop_done)) reader_task(NULL);
    assert(ticks == 4 && frames == 3 && down_count == 1);
    owner = 0; s_running = false; connected = false;
    vSemaphoreDelete(s_tx_lock);s_tx_lock=NULL;fake_xport.close(&fake_xport);
    fail_driver = true; assert(!cable_link_start(on_frame,session_tick,NULL));
    assert(!s_running && !driver && !s_tx_lock);
    fail_driver = false; fail_reader = true;
    assert(!cable_link_start(on_frame,session_tick,NULL));
    assert(!s_running && !driver && !s_tx_lock);
    free(s_agents);
    vSemaphoreDelete(s_agents_lock);vSemaphoreDelete(s_models_sem);vTaskDelete(hello_handle);
    assert(!live&&!hellos);
    puts("USB session: seven startup failures/retries, idempotence, early welcome, idle/error reads, timeout/reconnect and 64-bit wrap PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='harness-cable-session-') as folder:
    out = Path(folder)
    (out / 'session.c').write_text(code)
    # (TASK_STACK_PSRAM_FLASH_OPS, TASK_STACK_PSRAM_READER): the defaults of a product build (OTA off), the OTA build, and the reader alone
    for psram_stack, psram_reader in ((1, 1), (1, 0), (0, 0), (0, 1)):
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', f'-DTASK_STACK_PSRAM_FLASH_OPS={psram_stack}',
                        f'-DTASK_STACK_PSRAM_READER={psram_reader}',
                        '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                        '-I', str(main), str(out / 'session.c'), '-o', str(out / 'session')], check=True)
        subprocess.run([str(out / 'session')], check=True)

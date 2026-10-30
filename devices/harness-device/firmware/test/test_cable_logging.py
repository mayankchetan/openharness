"""Exercise production log framing while the USB writer is busy or re-enters, and (Wi-Fi mode) that the hook only queues."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

main = Path(__file__).resolve().parent / '../main'
source = (main / 'cable_link.c').read_text()

def function(name):
    match = re.search(r'^[^\n]*\b' + name + r'\([^;]*?\)\n\{.*?^\}', source, re.M | re.S)
    assert match, name
    return match.group(0) + '\n'

code = r'''
#include "cable_frame.h"
#include "cable_xport.h"
#include "log_ring.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define LOG_WRITE_WAIT_MS 5
#define LOG_LINE_MAX 512
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
typedef int TickType_t;
typedef int SemaphoreHandle_t;
typedef int (*vprintf_like_t)(const char *,va_list);
static bool isr, lock_busy, recurse, stalled;
static atomic_bool s_running, s_log_framing, s_async_logs;
static log_ring_t s_log_ring;
static uint8_t ring_store[1024];
static bool forbid_write;
#define EXPECT_LOCK() (atomic_load(&s_async_logs) ? 2 : 1)
static atomic_uint s_dropped_logs;
static int owner, s_tx_lock=1, s_fmt_lock=2, taken, plain_calls, framed, installed;
static uint8_t s_tx_frame[CABLE_MAX_FRAME], wire[8192];
static size_t wire_bytes;
static char saved[LOG_LINE_MAX];
static vprintf_like_t hook;
static int plain(const char *fmt,va_list args) { (void)fmt; (void)args;plain_calls++;return 1; }
static _Atomic(vprintf_like_t) s_prev_vprintf=plain;
static bool xPortInIsrContext(void) { return isr; }
static int xTaskGetCurrentTaskHandle(void) { return 9; }
static int xSemaphoreGetMutexHolder(int lock) { assert(lock==EXPECT_LOCK());return owner; }
static bool xSemaphoreTake(int lock,int wait) {
    assert(lock==EXPECT_LOCK()&&wait==5&&owner!=9);taken++;
    if(lock_busy)return false;
    assert(!owner);owner=9;return true;
}
static void xSemaphoreGive(int lock) { assert(lock==EXPECT_LOCK()&&owner==9);owner=0; }
static int log_vprintf(const char *fmt,va_list args);
static int log_line(const char *fmt,...) {
    va_list args;va_start(args,fmt);int n=log_vprintf(fmt,args);va_end(args);return n;
}
// The fake transport: the log path reaches the wire only through the vtable's write.
static const cable_xport_t *s_xport;
static int fake_write(const cable_xport_t *x,const uint8_t *p,size_t len,int wait) {
    assert(!forbid_write);   /* Wi-Fi mode: the hook must never reach the transport */
    assert(x==s_xport&&owner==9&&wait==5);
    if(recurse){recurse=false;assert(log_line("recursive driver log")==0);}
    if(stalled)return 0;
    assert(wire_bytes+len<=sizeof wire);memcpy(wire+wire_bytes,p,len);wire_bytes+=len;return (int)len;
}
static const cable_xport_t fake_xport = { "fake", CABLE_XPORT_USB, NULL, NULL, NULL, fake_write, NULL, NULL, NULL };
static void last_words_add(const char *p,size_t n) { assert(n<sizeof saved);memcpy(saved,p,n);saved[n]=0; }
static vprintf_like_t esp_log_set_vprintf(vprintf_like_t next) {
    vprintf_like_t old=hook;hook=next;installed++;return old;
}
'''
code += function('send_locked') + function('log_vprintf') + function('cable_link_set_log_framing')
code += r'''
static void received(uint8_t version,uint8_t type,const uint8_t *p,size_t n,void *ctx) {
    (void)ctx;assert(version==CABLE_FRAME_VERSION&&type==CABLE_TYPE_LOG);
    if(framed==0){assert(n==7&&!memcmp(p,"hello 7",n));}
    else if(framed==1){assert(n==5&&!memcmp(p,"outer",n));}
    else {assert(n==511);for(size_t i=0;i<n;i++)assert(p[i]=='x');}
    framed++;
}
int main(void) {
    hook=plain;s_running=true;s_xport=&fake_xport;
    cable_link_set_log_framing(true);assert(hook==log_vprintf&&installed==1);
    cable_link_set_log_framing(true);assert(installed==1);
    assert(log_line("hello %d\r\n",7)==9&&!strcmp(saved,"hello 7"));
    assert(!plain_calls&&!atomic_load(&s_dropped_logs)&&!owner);
    lock_busy=true;owner=8;assert(!log_line("congested"));assert(!plain_calls);
    lock_busy=false;owner=9;int before=taken;assert(!log_line("already owns tx"));
    assert(taken==before);owner=0;
    isr=true;assert(!log_line("ISR"));assert(taken==before);isr=false;
    recurse=true;assert(log_line("outer")==5);assert(atomic_load(&s_dropped_logs)==3);
    stalled=true;before=(int)wire_bytes;assert(log_line("not draining")==12);
    assert((int)wire_bytes==before&&!plain_calls&&!strcmp(saved,"not draining"));stalled=false;
    assert(atomic_load(&s_dropped_logs)==4);
    char long_line[2048];memset(long_line,'x',sizeof long_line-1);long_line[sizeof long_line-1]=0;
    assert(log_line("%s",long_line)==2047&&strlen(saved)==511);
    cable_decoder_t decoder;cable_decoder_init(&decoder);
    cable_decoder_feed(&decoder,wire,wire_bytes,received,NULL);
    assert(framed==3&&!decoder.corrupt_frames&&!decoder.discarded_bytes);
    cable_link_set_log_framing(false);assert(hook==plain&&installed==2);
    // A previously captured hook may still execute after removal on another core.
    assert(log_line("late callback")==1&&plain_calls==1);
    cable_link_set_log_framing(false);assert(installed==2);
    cable_link_set_log_framing(true);assert(hook==log_vprintf&&installed==3);
    puts("USB logging: bounded contention, same-task recursion, ISR, late hook, truncation; exact framed bytes PASS");

    // Wi-Fi mode: the same hook, but the line goes into the ring under the formatting lock. The transport is never written (fake_write asserts),
    // the tx lock is never taken (EXPECT_LOCK), and the ring hands the lines back in order.
    cable_link_set_log_framing(false);
    atomic_store(&s_async_logs,true);log_ring_init(&s_log_ring,ring_store,sizeof ring_store);
    forbid_write=true;owner=0;taken=0;plain_calls=0;wire_bytes=0;
    atomic_store(&s_dropped_logs,0);
    cable_link_set_log_framing(true);assert(hook==log_vprintf);
    assert(log_line("queued %d\r\n",1)==10&&!strcmp(saved,"queued 1"));
    assert(log_line("second")==6&&!strcmp(saved,"second"));
    assert(log_line("%s",long_line)==2047&&strlen(saved)==511);
    assert(!wire_bytes&&!plain_calls&&!owner&&taken==3&&!atomic_load(&s_dropped_logs));
    uint8_t out[LOG_RING_LINE_MAX];
    assert(log_ring_pop(&s_log_ring,out,sizeof out)==8&&!memcmp(out,"queued 1",8));
    assert(log_ring_pop(&s_log_ring,out,sizeof out)==6&&!memcmp(out,"second",6));
    assert(log_ring_pop(&s_log_ring,out,sizeof out)==511&&out[0]=='x'&&out[510]=='x');
    assert(!log_ring_pop(&s_log_ring,out,sizeof out));
    /* congestion and recursion still cost one line, never a wait or a transport write */
    lock_busy=true;owner=8;assert(!log_line("congested"));lock_busy=false;owner=0;
    assert(atomic_load(&s_dropped_logs)==1);
    owner=9;before=taken;assert(!log_line("already owns the format lock"));assert(taken==before&&atomic_load(&s_dropped_logs)==2);owner=0;
    isr=true;assert(!log_line("ISR"));isr=false;
    /* a stalled link does not matter: nothing is written while the ring fills, and its oldest lines go first */
    stalled=true;
    for(int i=0;i<400;i++)assert(log_line("line %04d padding padding padding padding",i)>0);
    assert(!wire_bytes&&log_ring_overwritten(&s_log_ring)>0&&log_ring_used(&s_log_ring)<=sizeof ring_store);
    unsigned popped=0;int last=-1;
    for(size_t n;(n=log_ring_pop(&s_log_ring,out,sizeof out));){int v;out[n]=0;assert(sscanf((char *)out,"line %d",&v)==1&&v>last);last=v;popped++;}
    assert(last==399&&popped+log_ring_overwritten(&s_log_ring)==400);
    cable_link_set_log_framing(false);
    puts("Wi-Fi logging: the hook queues in the ring, never writes the transport or takes the tx lock, drops oldest PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='harness-cable-log-') as folder:
    out = Path(folder)
    (out / 'log.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    '-pthread', '-I', str(main), str(out / 'log.c'), str(main / 'cable_frame.c'), str(main / 'log_ring.c'),
                    '-o', str(out / 'log')], check=True)
    subprocess.run([str(out / 'log')], check=True)

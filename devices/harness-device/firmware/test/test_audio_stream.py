"""Exercise actual capture/sender loops against an offline synthetic byte sink.

Two builds of the same sender source: CABLE_VOICE_ADPCM_BUILT=0 (the PCM sender every image has today) and =1 (the ADPCM
path of task W4, which the sender takes only when the turn's codec was chosen ADPCM). The ADPCM sink decodes every block.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

main = Path(__file__).resolve().parent / '../main'
source = Path(os.environ.get('AUDIO_STREAM_SOURCE', main / 'audio_client_habitat.c')).read_text()

def function(name):
    match = re.search(r'^static void ' + name + r'\([^;]*?\)\n\{.*?^\}', source, re.M | re.S)
    assert match, name
    return match.group(0) + '\n'

code = r'''
#include "voice_buffer.h"
#include "adpcm_ima.h"
#include <assert.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#define CAPTURE_SAMPLES 160
#define BUFFER_BYTES 32768
#define AUDIO_SAMPLE_RATE 16000
#define CABLE_VOICE_CHUNK 1024
#define CFG_VLANG_MAX 16
#define EXT_RAM_BSS_ATTR
#ifndef CABLE_VOICE_ADPCM_BUILT
#define CABLE_VOICE_ADPCM_BUILT 0
#endif
#define pdTRUE 1
#define portMAX_DELAY (-1)
#define pdMS_TO_TICKS(ms) (ms)
static const char *TAG="offline-test";
static void log_noop(const char *tag,const char *format,...) {(void)tag;(void)format;}
#define ESP_LOGI(...) log_noop(__VA_ARGS__)
#define ESP_LOGE(...) log_noop(__VA_ARGS__)
typedef void *TaskHandle_t;
static int capture_token, sender_token;
static TaskHandle_t capture_task=&capture_token,sender_task=&sender_token;
static voice_buffer_t buffer;
static atomic_bool active, recording, stop_requested, abort_requested, capture_done, heard;
static atomic_uint input_level;
static char agent[]="target-agent";
enum { VOICE_CMD_NONE,VOICE_CMD_GOAL,VOICE_CMD_LOOP };
static int command;
static uint32_t produced,high_water,read_max_us;
enum { HEALTHY, BEGIN_FAIL, START_FAIL, READ_FAIL, READ_RECOVER, TX_FAIL, OVERFLOW, DMA_LOSS, MODES };
static int mode,role;
static unsigned capture_waits,sender_waits,reads,raw_bytes,sent_bytes,pcm_calls,reads_total=64;
static unsigned adpcm_seq,adpcm_samples,adpcm_last_index_end;
static bool adpcm_seen_last;
static unsigned starts,stops,begins,ends,aborts,notifications,overruns;
static uint64_t clock_us;
static jmp_buf captured,finished;
static void capture(void *unused);
static int64_t esp_timer_get_time(void) {return (int64_t)(clock_us+=100);}
static unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) {(void)task;return 4096;}
static int ulTaskNotifyTake(int clear,int wait) {
    assert(clear==pdTRUE&&wait==portMAX_DELAY);
    if(role==1) {if(capture_waits++)longjmp(captured,1);}
    else {if(sender_waits++){assert(!active);longjmp(finished,1);}}
    return 1;
}
static void xTaskNotifyGive(TaskHandle_t task) {
    notifications++;
    if(task==sender_task)return;
    assert(task==capture_task&&role==2);role=1;
    if(!setjmp(captured))capture(NULL);
    role=2;
}
static void vTaskDelay(unsigned ticks) {clock_us+=ticks*1000;}
static bool audio_capture_start(void) {starts++;return mode!=START_FAIL;}
static int audio_capture_read(uint8_t *out,int capacity) {
    assert(capacity==CAPTURE_SAMPLES*2);reads++;
    if(mode==READ_FAIL||(mode==READ_RECOVER&&reads<=2))return -1;
    for(int i=0;i<capacity;i++)out[i]=(uint8_t)(raw_bytes+i);
    raw_bytes+=(unsigned)capacity;
    if(mode==DMA_LOSS&&reads==3)overruns++;
    if(mode!=OVERFLOW&&raw_bytes==reads_total*CAPTURE_SAMPLES*2)atomic_store(&stop_requested,true);
    return capacity;
}
static unsigned audio_capture_overruns(void) {return overruns;}
static void audio_capture_stop(void) {stops++;}
static void config_load_voicelang(char *out,size_t n) {snprintf(out,n,"en");}
static bool audio_stream_begin(const char *id,const char *cmd,const char *lang,int rate) {
    assert(id&&!strcmp(id,"target-agent")&&!strcmp(cmd,"")&&!strcmp(lang,"en")&&rate==16000);
    begins++;return mode!=BEGIN_FAIL;
}
static bool audio_stream_pcm(const uint8_t *bytes,size_t n) {
    assert(begins==1&&mode!=BEGIN_FAIL&&n&&n<=CABLE_VOICE_CHUNK);pcm_calls++;
    for(size_t i=0;i<n;i++)assert(bytes[i]==(uint8_t)(sent_bytes+i));
    if(mode==TX_FAIL&&pcm_calls==3)return false;
    sent_bytes+=(unsigned)n;return true;
}
__attribute__((unused)) static bool audio_stream_is_adpcm(void) {return CABLE_VOICE_ADPCM_BUILT;}
#define TURN_TAG 0x5C
__attribute__((unused)) static uint8_t audio_stream_voice_tag(void) {return TURN_TAG;}   // the tag voice.begin carried for this turn
// The ADPCM sink: every block is decoded and checked against the capture pattern (byte g of the turn is (uint8_t)g).
__attribute__((unused)) static bool audio_stream_adpcm(const uint8_t *bytes,size_t n) {
    assert(begins==1&&mode!=BEGIN_FAIL&&n&&n<=ADPCM_PAYLOAD_MAX);pcm_calls++;
    adpcm_info_t info;int16_t out[ADPCM_BLOCK_SAMPLES];
    int got=adpcm_decode_block(bytes,n,out,ADPCM_BLOCK_SAMPLES,&info);
    assert(got>0&&info.seq==adpcm_seq&&!adpcm_seen_last);
    assert(info.tag==TURN_TAG&&bytes[8]==TURN_TAG);   // every block of the turn names the turn
    assert(adpcm_seq==0||info.index_start==adpcm_last_index_end);   // the step index is carried across blocks
    adpcm_last_index_end=info.index_end;
    unsigned first=adpcm_samples;   // sample 0 of a block is verbatim: the order and the cut points are exact
    assert(out[0]==(int16_t)((uint16_t)(uint8_t)(2*first)|(uint16_t)((uint16_t)(uint8_t)(2*first+1)<<8)));
    if(info.flags&ADPCM_FLAG_LAST)adpcm_seen_last=true;else assert(got==ADPCM_BLOCK_SAMPLES);
    if(mode==TX_FAIL&&pcm_calls==3)return false;
    adpcm_seq++;adpcm_samples+=(unsigned)got;sent_bytes+=(unsigned)got*2;return true;
}
static void audio_stream_end(void) {
    assert(capture_done&&!abort_requested&&sent_bytes==produced);ends++;
}
static void audio_stream_abort(const char *why) {assert(why&&capture_done);aborts++;}
'''
code += function('capture')
code += '#if CABLE_VOICE_ADPCM_BUILT\n' + function('send_adpcm') + '#endif\n'
code += function('send_audio')
code += r'''
int main(void) {
    static uint8_t guarded[BUFFER_BYTES+16];
    static unsigned shape;   // static: a local loop variable here draws -Wclobbered (setjmp) on some compilers
    for(unsigned round=0;round<100;round++)for(mode=0;mode<MODES;mode++)for(shape=0;shape<2;shape++) {
        reads_total=shape?101:64;   // 101 reads = 32,320 B = exactly 32 full ADPCM blocks; 64 reads = 20 full blocks + 140 samples
        adpcm_seq=adpcm_samples=adpcm_last_index_end=0;adpcm_seen_last=false;
        memset(guarded,0xa5,sizeof guarded);voice_buffer_init(&buffer,guarded+8,BUFFER_BYTES);
        if(round&1) {atomic_store(&buffer.head,UINT32_MAX-255);atomic_store(&buffer.tail,UINT32_MAX-255);}
        active=recording=true;stop_requested=abort_requested=capture_done=heard=false;
        input_level=produced=high_water=read_max_us=0;
        capture_waits=sender_waits=reads=raw_bytes=sent_bytes=pcm_calls=0;
        starts=stops=begins=ends=aborts=notifications=overruns=0;clock_us=0;role=2;
        if(!setjmp(finished))send_audio(NULL);
        assert(!active&&!recording&&capture_done&&input_level==0&&begins==1&&starts==1);
        assert(stops==(unsigned)(mode!=START_FAIL));
        assert(high_water<=BUFFER_BYTES);
        if(mode==HEALTHY||mode==READ_RECOVER) {
            assert(ends==1&&!aborts&&sent_bytes==reads_total*CAPTURE_SAMPLES*2&&!voice_buffer_used(&buffer));
            if(CABLE_VOICE_ADPCM_BUILT) {
                unsigned samples=sent_bytes/2;
                assert(adpcm_samples==samples&&adpcm_seq==(samples+ADPCM_BLOCK_SAMPLES-1)/ADPCM_BLOCK_SAMPLES);
                assert(adpcm_seq==pcm_calls&&adpcm_seen_last);   // the last block, short or full, carries the flag
                assert(adpcm_seq==(shape?32u:21u));
            }
        } else {
            assert(!ends&&aborts==1);
            if(mode!=TX_FAIL)assert(!pcm_calls);
        }
        for(int i=0;i<8;i++)assert(guarded[i]==0xa5&&guarded[BUFFER_BYTES+8+i]==0xa5);
    }
    puts(CABLE_VOICE_ADPCM_BUILT ? "Audio workers (ADPCM sender): 1600 offline sessions; every block decoded, exact cut points, carried index, flagged last block, abort paths PASS"
        : "Audio workers: 1600 offline sessions; exact PCM order, failed begin/start, read retry/failure, USB failure, ring overflow, DMA loss and 32-bit wrap PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='harness-audio-stream-') as folder:
    out = Path(folder)
    (out / 'stream.c').write_text(code)
    for built in (0, 1):
        exe = out / f'stream{built}'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', f'-DCABLE_VOICE_ADPCM_BUILT={built}',
                        '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                        '-I', str(main), str(out / 'stream.c'), str(main / 'voice_buffer.c'), str(main / 'adpcm_ima.c'),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)

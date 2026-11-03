"""Exercise actual capture/sender loops against an offline synthetic byte sink.

Two builds of the same sender source: CABLE_VOICE_ADPCM_BUILT=0 (the PCM sender every image has today) and =1 (the ADPCM
path of task W4, which the sender takes only when the turn's codec was chosen ADPCM). The ADPCM sink decodes every block.

Task W12 adds, to both builds: the stall cases of the Wi-Fi ring (a sender that cannot send for 1.5 s survives with the 96 KB ring and not with the
32 KB USB ring; one that cannot send for 4 s overflows either ring, logs the existing message once and aborts once); a socket that dies mid-turn
(one abort, no send after the failure, even when the UI aborts as well); and the codec rule as the glue in cable_client.c applies it, for each
transport (ADPCM needs the daemon's feature AND a non-USB transport, or the USB build flag; USB is PCM).
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
#define BUFFER_BYTES_WIFI 98304   /* the Wi-Fi ring (W12): 3 x 2^15, 3.07 s of PCM */
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
static unsigned overflow_logs;   /* how often the existing "ring full" message was logged */
static void log_noop(const char *tag,const char *format,...) {
    (void)tag;if(strstr(format,"stalled beyond the PCM buffer; discarding utterance"))overflow_logs++;
}
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
enum { HEALTHY, BEGIN_FAIL, START_FAIL, READ_FAIL, READ_RECOVER, TX_FAIL, OVERFLOW, DMA_LOSS, SOCKET_LOSS, MODES };
static bool link_lost;   /* a send failed: the link is gone, and nothing may be handed to it again in this turn */
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
#define VOICE_LINK_WAIT_MS 2800
static bool cable_client_wait_session(uint32_t ms) { (void)ms; return true; }   /* no radio-off in this harness: the link is always there */
static bool audio_stream_begin(const char *id,const char *cmd,const char *lang,int rate) {
    assert(id&&!strcmp(id,"target-agent")&&!strcmp(cmd,"")&&!strcmp(lang,"en")&&rate==16000);
    begins++;return mode!=BEGIN_FAIL;
}
static bool audio_stream_pcm(const uint8_t *bytes,size_t n) {
    assert(begins==1&&mode!=BEGIN_FAIL&&n&&n<=CABLE_VOICE_CHUNK&&!link_lost);pcm_calls++;
    for(size_t i=0;i<n;i++)assert(bytes[i]==(uint8_t)(sent_bytes+i));
    if((mode==TX_FAIL||mode==SOCKET_LOSS)&&pcm_calls==3) {
        link_lost=true;
        if(mode==SOCKET_LOSS)atomic_store(&abort_requested,true);   /* the UI aborts the turn too, as it does when the link drops */
        return false;
    }
    sent_bytes+=(unsigned)n;return true;
}
__attribute__((unused)) static bool audio_stream_is_adpcm(void) {return CABLE_VOICE_ADPCM_BUILT;}
#define TURN_TAG 0x5C
__attribute__((unused)) static uint8_t audio_stream_voice_tag(void) {return TURN_TAG;}   // the tag voice.begin carried for this turn
// The ADPCM sink: every block is decoded and checked against the capture pattern (byte g of the turn is (uint8_t)g).
__attribute__((unused)) static bool audio_stream_adpcm(const uint8_t *bytes,size_t n) {
    assert(begins==1&&mode!=BEGIN_FAIL&&n&&n<=ADPCM_PAYLOAD_MAX&&!link_lost);pcm_calls++;
    adpcm_info_t info;int16_t out[ADPCM_BLOCK_SAMPLES];
    int got=adpcm_decode_block(bytes,n,out,ADPCM_BLOCK_SAMPLES,&info);
    assert(got>0&&info.seq==adpcm_seq&&!adpcm_seen_last);
    assert(info.tag==TURN_TAG&&bytes[8]==TURN_TAG);   // every block of the turn names the turn
    assert(adpcm_seq==0||info.index_start==adpcm_last_index_end);   // the step index is carried across blocks
    adpcm_last_index_end=info.index_end;
    unsigned first=adpcm_samples;   // sample 0 of a block is verbatim: the order and the cut points are exact
    assert(out[0]==(int16_t)((uint16_t)(uint8_t)(2*first)|(uint16_t)((uint16_t)(uint8_t)(2*first+1)<<8)));
    if(info.flags&ADPCM_FLAG_LAST)adpcm_seen_last=true;else assert(got==ADPCM_BLOCK_SAMPLES);
    if((mode==TX_FAIL||mode==SOCKET_LOSS)&&pcm_calls==3) {
        link_lost=true;
        if(mode==SOCKET_LOSS)atomic_store(&abort_requested,true);
        return false;
    }
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
        starts=stops=begins=ends=aborts=notifications=overruns=0;clock_us=0;role=2;link_lost=false;
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
            if(mode!=TX_FAIL&&mode!=SOCKET_LOSS)assert(!pcm_calls);
            if(mode==SOCKET_LOSS)assert(pcm_calls==3&&link_lost);   /* the failed send was the last: nothing after it */
        }
        for(int i=0;i<8;i++)assert(guarded[i]==0xa5&&guarded[BUFFER_BYTES+8+i]==0xa5);
    }
    /* W12: a stall. The capture loop of this harness runs to its stop before the sender gets a turn, so reads_total reads (10 ms and 320 B each) are in the
       ring when the first frame can leave: that is a sender whose link takes nothing for reads_total x 10 ms. */
    static uint8_t wifi_ring[BUFFER_BYTES_WIFI+16];
    static const struct { unsigned ms, ring; bool survives; } stalls[]={
        {1500,BUFFER_BYTES_WIFI,true},    /* G6: the Wi-Fi ring rides out a 1.5 s stall: no overflow, no abort */
        {1500,BUFFER_BYTES,false},        /* the USB ring (1.02 s) would not: the reason the Wi-Fi ring is bigger */
        {3000,BUFFER_BYTES_WIFI,true},    /* 96,000 B: the ring holds 3.07 s */
        {4000,BUFFER_BYTES_WIFI,false},   /* 4 s overflows it: the turn aborts with the existing message, once */
        {4000,BUFFER_BYTES,false},
    };
    static unsigned stall_cases,stall_round;   /* static: setjmp below, and -Wclobbered */
    for(stall_round=0;stall_round<20;stall_round++)for(unsigned s=0;s<sizeof stalls/sizeof *stalls;s++) {
        const unsigned ring=stalls[s].ring;mode=HEALTHY;
        reads_total=stalls[s].ms/10;
        adpcm_seq=adpcm_samples=adpcm_last_index_end=0;adpcm_seen_last=false;overflow_logs=0;
        memset(wifi_ring,0xa5,sizeof wifi_ring);voice_buffer_init(&buffer,wifi_ring+8,ring);
        if(stall_round&1) {   /* the counters of a ring that is not a power of two wrap at span, not at 2^32 */
            uint32_t at=buffer.span?buffer.span-255:UINT32_MAX-255;atomic_store(&buffer.head,at);atomic_store(&buffer.tail,at);
        }
        active=recording=true;stop_requested=abort_requested=capture_done=heard=false;
        input_level=produced=high_water=read_max_us=0;
        capture_waits=sender_waits=reads=raw_bytes=sent_bytes=pcm_calls=0;
        starts=stops=begins=ends=aborts=notifications=overruns=0;clock_us=0;role=2;link_lost=false;
        if(!setjmp(finished))send_audio(NULL);
        assert(!active&&!recording&&capture_done&&begins==1&&starts==1&&stops==1&&high_water<=ring);
        if(stalls[s].survives) {
            assert(!overflow_logs&&ends==1&&!aborts&&sent_bytes==reads_total*CAPTURE_SAMPLES*2&&!voice_buffer_used(&buffer));
            assert(high_water==sent_bytes);                              /* everything sat in the ring at once ... */
            if(ring==BUFFER_BYTES_WIFI)assert(high_water>BUFFER_BYTES);  /* ... which the 32 KB ring could not have held */
            if(stalls[s].ms==1500)assert(high_water*2<ring);             /* G6: ring_peak under 50% of 96 KB at a 1.5 s stall */
        } else {
            assert(overflow_logs==1&&!ends&&aborts==1&&!pcm_calls);   /* the existing message once, one voice.abort, no frame sent */
        }
        for(int i=0;i<8;i++)assert(wifi_ring[i]==0xa5&&wifi_ring[ring+8+i]==0xa5);
        stall_cases++;
    }
    puts(CABLE_VOICE_ADPCM_BUILT ? "Audio workers (ADPCM sender): 1800 offline sessions; every block decoded, exact cut points, carried index, flagged last block, abort paths PASS"
        : "Audio workers: 1800 offline sessions; exact PCM order, failed begin/start, read retry/failure, USB failure, socket loss, ring overflow, DMA loss and 32-bit wrap PASS");
    printf("Audio stall cases: %u (1.5 s and 3 s survive the 96 KB ring, 4 s aborts once with the existing message, 1.5 s overflows the 32 KB ring) PASS\n",stall_cases);
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

# -- the codec rule as cable_client.c applies it (W12) -------------------------------------------------------------
# The two functions are cut out of the real source and compiled against a fake transport kind and a fake feature word, so a
# glue that ignores the transport (the stub W4 left: "the only link is USB") fails here. voice.begin.codec is "adpcm" exactly when
# cable_client_voice_adpcm_wanted() is true; PCM is the absence of the field.
client = (main / 'cable_client.c').read_text()
transport_fn = re.search(r'^static bool voice_transport_is_usb\(void\)[^\n]*\n', client, re.M)
wanted_fn = re.search(r'^bool cable_client_voice_adpcm_wanted\(void\)\n\{.*?^\}', client, re.M | re.S)
assert transport_fn and wanted_fn, 'voice_transport_is_usb / cable_client_voice_adpcm_wanted not found in cable_client.c'
codec_code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include "cable_features.h"
#include "cable_xport.h"
static cable_xport_kind_t kind;
__attribute__((unused)) static cable_xport_kind_t cable_link_transport(void) { return kind; }
static atomic_uint s_features;
#ifndef DEVICE_VOICE_ADPCM_USB
#define DEVICE_VOICE_ADPCM_USB 0
#endif
''' + transport_fn[0] + wanted_fn[0] + r'''
int main(void) {
    static const cable_xport_kind_t kinds[] = {CABLE_XPORT_USB, CABLE_XPORT_WIFI, CABLE_XPORT_BLE};
    static const uint32_t words[] = {0, CABLE_FEATURE_ADPCM, ~0u, ~CABLE_FEATURE_ADPCM, CABLE_FEATURE_NET | CABLE_FEATURE_DRAFT};
    unsigned n = 0;
    for (unsigned k = 0; k < 3; k++) for (unsigned w = 0; w < sizeof words / sizeof *words; w++) {
        kind = kinds[k];
        atomic_store(&s_features, words[w]);
        const bool feature = (words[w] & CABLE_FEATURE_ADPCM) != 0;
        const bool expect = feature && (kinds[k] != CABLE_XPORT_USB || DEVICE_VOICE_ADPCM_USB != 0);
        assert(cable_client_voice_adpcm_wanted() == expect);
        n++;
    }
    kind = CABLE_XPORT_USB;atomic_store(&s_features, ~0u);
    assert(cable_client_voice_adpcm_wanted() == (DEVICE_VOICE_ADPCM_USB != 0));   /* USB: PCM unless the G1 build flag is on */
    kind = CABLE_XPORT_WIFI;atomic_store(&s_features, 0);
    assert(!cable_client_voice_adpcm_wanted());                                    /* Wi-Fi to a daemon without voice.adpcm: PCM */
    kind = CABLE_XPORT_WIFI;atomic_store(&s_features, CABLE_FEATURE_ADPCM);
    assert(cable_client_voice_adpcm_wanted());                                     /* Wi-Fi to a daemon with it: ADPCM */
    printf("Voice codec choice (DEVICE_VOICE_ADPCM_USB=%d): %u transport x feature cases PASS\n", DEVICE_VOICE_ADPCM_USB, n);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-audio-codec-') as folder:
    out = Path(folder)
    (out / 'codec.c').write_text(codec_code)
    for usb_flag in (0, 1):
        exe = out / f'codec{usb_flag}'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', f'-DDEVICE_VOICE_ADPCM_USB={usb_flag}',
                        '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                        '-I', str(main), str(out / 'codec.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)

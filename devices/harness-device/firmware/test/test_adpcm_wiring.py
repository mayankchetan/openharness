"""Source wiring of the ADPCM sender (task W4): the build flags, the hello/voice.begin fields, the protocol version, the
seam in audio_probe.h, the PSRAM block buffer and the VOX probe hooks. The sender LOGIC runs in test_audio_stream.py and the
codec choice rule in test_cable_features.c; this file only pins that the pieces are connected, and that with the flag off
every ADPCM line sits inside a preprocessor guard (so the image sends PCM exactly as before)."""
from pathlib import Path
import re

main = Path(__file__).resolve().parent / '../main'


def text(name):
    return (main / name).read_text()


def strip_guarded(src):
    """Remove every `#if CABLE_VOICE_ADPCM_BUILT` / `#ifdef DEVICE_VOX_PROBE` block (nesting-aware), as the preprocessor
    does with the flags off."""
    out, depth, skipping = [], 0, False
    for line in src.splitlines():
        s = line.strip()
        if not skipping and re.match(r'#\s*if(def)?\s+(CABLE_VOICE_ADPCM_BUILT|DEVICE_VOX_PROBE)\b', s):
            skipping, depth = True, 1
            continue
        if skipping:
            if re.match(r'#\s*if', s):
                depth += 1
            elif re.match(r'#\s*endif', s):
                depth -= 1
                skipping = depth > 0
            continue
        out.append(line)
    assert not skipping, 'unbalanced #if'
    return '\n'.join(out)


# -- build system ---------------------------------------------------------------------------------------------------
cmake = text('CMakeLists.txt')
assert '"adpcm_ima.c"' in cmake.split('set(BOARD_REQUIRES')[0], 'adpcm_ima.c is in the source list'
assert re.search(r'if\(DEVICE_VOICE_ADPCM_USB\)\s+target_compile_definitions\(\$\{COMPONENT_LIB\} PRIVATE DEVICE_VOICE_ADPCM_USB=1\)', cmake)
vox = re.search(r'if\(DEVICE_VOX_PROBE\)(.*?)\nendif\(\)', cmake, re.S)
assert vox, 'DEVICE_VOX_PROBE block'
assert 'DEVICE_VOX_PROBE=1' in vox[1] and '"vox_probe.c"' in vox[1] and 'DEVICE_VOX_REVIEW=1' in vox[1]
assert '"vox_probe.c"' not in cmake.split('if(DEVICE_VOX_PROBE)')[0], 'the probe is not in the default source list'

# -- protocol version and the hello / voice.begin fields -----------------------------------------------------------
header = text('cable_client.h')
assert re.search(r'#define CABLE_PROTO_VERSION 4\b', header)
assert re.search(r'#ifndef DEVICE_VOICE_ADPCM_USB\s+#define DEVICE_VOICE_ADPCM_USB 0\s+#endif', header)
assert re.search(r'#define CABLE_VOICE_ADPCM_BUILT\s+DEVICE_VOICE_ADPCM_USB', header)
for decl in ('bool cable_client_voice_adpcm(const uint8_t *payload, size_t len);', 'bool cable_client_voice_is_adpcm(void);',
             'bool cable_client_voice_adpcm_wanted(void);', 'uint8_t cable_client_voice_tag(void);'):
    assert decl in header, decl
client = text('cable_client.c')
hello = re.search(r'static void send_hello\(void\)\n\{.*?\n\}', client, re.S)[0]
assert re.search(r'#if CABLE_VOICE_ADPCM_BUILT\s+(?://[^\n]*\n\s*)*if \(cable_voice_adpcm_allowed\([^\n]*\)\s+msg_string\(&root, "voiceCodec", "adpcm-v1"\);\s+#endif', hello)
begin = re.search(r'bool cable_client_voice_begin\(.*?\n\}', client, re.S)[0]
assert re.search(r'#if CABLE_VOICE_ADPCM_BUILT\s+.*cable_client_voice_adpcm_wanted\(\).*s_voice_adpcm.*#endif', begin, re.S)
# The codec and the turn's tag go out together, and the tag counter moves at every voice.begin, ADPCM turn or not.
assert re.search(r'if \(adpcm\) \{\s+msg_string\(&root, "codec", "adpcm"\);\s+msg_number\(&root, "tag", tag\);\s+\}', begin), 'voice.begin carries codec and tag together'
assert re.search(r'const uint8_t tag = \(uint8_t\)\(atomic_fetch_add\(&s_voice_tag, 1\) \+ 1\);', begin), 'a new tag at every voice.begin'
assert re.search(r'uint8_t cable_client_voice_tag\(void\) \{ return \(uint8_t\)atomic_load\(&s_voice_tag\); \}', client)
assert re.search(r'#if CABLE_VOICE_ADPCM_BUILT\s+bool cable_client_voice_adpcm\(const uint8_t \*payload, size_t len\)\s+\{\s+return cable_link_send\(CABLE_TYPE_ADPCM, payload, len\);', client)

# -- the seam (audio_probe.h) ---------------------------------------------------------------------------------------
seam = text('audio_probe.h')
assert '#define audio_stream_adpcm cable_client_voice_adpcm' in seam and '#define audio_stream_is_adpcm cable_client_voice_is_adpcm' in seam
assert '#define audio_stream_voice_tag cable_client_voice_tag' in seam and 'uint8_t audio_stream_voice_tag(void);' in seam
assert 'bool audio_stream_adpcm(' in seam and 'bool audio_stream_is_adpcm(void);' in seam   # the DEVICE_PERF_BENCH declarations
probe = text('audio_probe.c')
assert 'bool audio_stream_adpcm(' in probe and 'bool audio_stream_is_adpcm(void)' in probe and 'uint8_t audio_stream_voice_tag(void)' in probe

# -- the sender -----------------------------------------------------------------------------------------------------
sender = text('audio_client_habitat.c')
assert re.search(r'#if CABLE_VOICE_ADPCM_BUILT\s+#include "adpcm_ima.h"\s+(?:#include [^\n]*\s+)*#endif', sender)
fn = re.search(r'#if CABLE_VOICE_ADPCM_BUILT\n(?://[^\n]*\n)*static void send_adpcm\([^;]*?\)\n\{.*?^\}\n#endif', sender, re.M | re.S)
assert fn, 'send_adpcm sits inside #if CABLE_VOICE_ADPCM_BUILT'
body = fn[0]
assert re.search(r'static EXT_RAM_BSS_ATTR struct \{[^}]*int16_t pcm\[ADPCM_BLOCK_SAMPLES\];[^}]*uint8_t out\[ADPCM_PAYLOAD_MAX\];', body, re.S), \
    'the block buffer is a static in PSRAM, not on the 5,120 B voice_send stack'
locals_only = re.sub(r'static EXT_RAM_BSS_ATTR struct \{.*?\} blk;', '', body, flags=re.S)
stack_arrays = re.findall(r'^\s+(?:u?int\d+_t|char)\s+\w+\[[^\]]+\];', locals_only, re.M)
assert not stack_arrays, stack_arrays
assert 'adpcm_encode_block(' in body and 'audio_stream_adpcm(' in body and 'ADPCM_FLAG_LAST' in body
# The tag is read once per turn, before the loop, and goes to every block (the reserved byte of the WAV block).
assert re.search(r'const uint8_t tag = audio_stream_voice_tag\(\);.*for \(;;\)', body, re.S)
assert re.search(r'adpcm_encode_block\(blk\.pcm, samples, &index, seq, flags, tag, blk\.out, sizeof blk\.out\)', body)
send = re.search(r'static void send_audio\(void \*unused\)\n\{.*?^\}', sender, re.M | re.S)[0]
assert re.search(r'#if CABLE_VOICE_ADPCM_BUILT\s+.*?audio_stream_is_adpcm\(\);\s+if \(adpcm\)\s+send_adpcm\(&delivered, &send_max_us, &encode_us_max, &adpcm_blocks\);\s+else\s+#endif\s+for \(;;\) \{', send, re.S)
assert re.search(r'#if CABLE_VOICE_ADPCM_BUILT\s+if \(adpcm\)\s+ESP_LOGI\(TAG, "codec=adpcm blocks=%lu encode_us_max=%lu"', send)
assert 'audio_stream_pcm(data, n)' in send    # the PCM loop is still there, untouched

# -- with the flags off, every ADPCM and probe line is inside a guard: the PCM text is what it was ----------------
flat = strip_guarded(sender)
for word in ('adpcm', 'ADPCM', 'encode_us_max', 'vox_'):
    assert word not in flat, f'audio_client_habitat.c: unguarded {word}'
flat = strip_guarded(text('app_main.c'))
assert 'vox_' not in flat

# -- the VOX probe --------------------------------------------------------------------------------------------------
vp = text('vox_probe.c')
assert '#include "vox_gate.h"' in vp and 'DEVICE_VOX_REVIEW' in vp
assert 'codec=%s' in vp and 'encode_us_max=%u' in vp and 'blocks=%u' in vp
assert 'codec=pcm16' not in vp and 'encode_us_max=0' not in vp, 'codec and encode time are measured, not constants'
assert re.search(r'#ifdef DEVICE_VOX_PROBE\s+(?:\{\s+)?(?:extern void vox_start\(void\);\s+)?vox_start\(\);', text('app_main.c')) or 'vox_start' in text('app_main.c')
assert re.search(r'#ifdef DEVICE_VOX_PROBE\s+if \(!strcmp\(t, "voice.transcript"\)', client)
assert re.search(r'#ifdef DEVICE_VOX_PROBE\s+(?:#if CABLE_VOICE_ADPCM_BUILT\s+)?vox_on_sent\(', sender)

print('ADPCM wiring: build flags, proto 4, hello.voiceCodec, voice.begin.codec, seam, PSRAM block buffer, VOX probe hooks; '
      'flag-off text has no ADPCM PASS')

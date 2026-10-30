"""Compile and run test_vox_gate.c: the safety gates of the VOX probe in review mode (main/vox_gate.h, pure C)."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='harness-vox-gate-') as directory:
    exe = Path(directory) / 'test_vox_gate'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-I', str(here / '../main'),
                    str(here / 'test_vox_gate.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# -- the probe's source: a review run must never send a transcript to an agent (task W4 fix) ---------------------------
probe = (here / '../main/vox_probe.c').read_text()
loop = probe[probe.index('for (int n = 1; n <= VOX_N; n++)'):]
start = loop.index('audio_client_start_cable(')
delay = loop.index('vTaskDelay(pdMS_TO_TICKS(VOX_CAPTURE_MS))')
stop = loop.index('audio_client_stop();')
flag = loop.index('audio_client_request_review();')
assert start < flag < delay < stop, 'the review flag is set right after the start, before the capture delay, not after it'
assert re.search(r'audio_client_start_cable\([^;]*\);\s+if \(review\) audio_client_request_review\(\);\s+vTaskDelay', loop), \
    'the flag is set before the delay, inside `if (review)`'
check = loop[delay:stop]
assert 'audio_client_review_requested()' in check, 'the post-check of the flag stays'
assert 'vox_stop_gate(VOX_REVIEW != 0, audio_client_review_requested(), audio_client_recording())' in check
assert re.search(r'if \(stop_why\) \{\s+audio_client_abort\(\);\s+ESP_LOGE\(TAG, "VOX ABORT reason=%s n=%d', check), 'a failed stop gate aborts the utterance and logs'
assert re.search(r'ESP_LOGE\(TAG, "VOX ABORT reason=%s n=%d[^;]*stop_why', check)
assert 'break;' in check[check.index('if (stop_why)'):], 'review_not_set ends the run'
# The reply type buffer holds "voice.transcript" (16 characters and a NUL) whole.
size = int(re.search(r's_reply_type\[(\d+)\]', probe)[1])
assert size >= len('voice.transcript') + 1, size
print('VOX probe source: review flag set right after the start, stop gate before audio_client_stop, reply type buffer', size)

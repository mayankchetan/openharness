"""B7: voice in the phone mode (see test_ble_voice.c).

Cuts the ring-size line out of main/audio_client_habitat.c, compiles test_ble_voice.c with it as RING_EXPR against the real config_store.c and the in-memory
NVS, and runs it. A change to the line that drops the phone mode (or hands the big ring to a mode that is not whole) fails the C run; a line that is not
found fails here.

The other half of B7's plan needs no new test: the codec rule already has its BLE row (test_audio_stream.py, 'Voice codec choice': the kinds USB, Wi-Fi
and BLE against every feature word), the stall cases (1.5 s and 3 s survive the 96 KB ring, 4 s aborts once) and the lost-link abort are properties of the
ring and the sender and do not depend on the wire, and cable_client.c voice_transport_is_usb() already reads "not USB" for BLE.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
MAIN = HERE / '../main'
source = (MAIN / 'audio_client_habitat.c').read_text(encoding='utf-8')

match = re.search(r'^\s*ring_bytes = (\(net\.mode == 1 \|\| net\.mode == 2\) \? BUFFER_BYTES_WIFI : BUFFER_BYTES);', source, re.M)
assert match, 'the ring-size line of audio_client_init is not the one the phone mode needs'
assert 'config_net_info(&net)' in source, 'the size comes from the stored mode'
# no other place picks the ring from the Wi-Fi flag alone
assert not re.search(r'net\.saved && net\.mode == 1', source)

with tempfile.TemporaryDirectory(prefix='harness-ble-voice-') as folder:
    exe = Path(folder) / 'ble_voice'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    f'-DRING_EXPR={match[1]}', '-I', str(MAIN), '-I', str(HERE / 'host_stubs'), '-I', str(HERE / 'host_nvs'),
                    str(HERE / 'test_ble_voice.c'), str(MAIN / 'cable_features.c'), str(HERE / 'host_nvs/host_nvs.c'), str(MAIN / 'config_store.c'),
                    '-I', str(Path(os.environ['IDF_PATH']) / 'components/json/cJSON'), str(Path(os.environ['IDF_PATH']) / 'components/json/cJSON/cJSON.c'),
                    '-Wno-deprecated-declarations', '-lm', '-o', str(exe)], check=True)
    done = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
    print(done.stdout.strip())

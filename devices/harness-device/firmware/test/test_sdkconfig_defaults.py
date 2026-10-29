"""The radio and memory symbols the RAM diet depends on are explicit in the tracked defaults files.

sdkconfig is generated and gitignored, and IDF reads the defaults files only when it creates it (the top-level
CMakeLists.txt re-seeds it when the defaults change). So this test checks two things:
  1. the defaults files (sdkconfig.defaults, then sdkconfig.defaults.esp32s3, later wins) state the values;
  2. if a generated config is available (env SDKCONFIG, else firmware/sdkconfig), it agrees with every symbol
     the defaults set. Without one, only part 1 runs and the output says so.
"""
from pathlib import Path
import os
import re

firmware = Path(__file__).resolve().parent / '..'


def read_defaults():
    values = {}
    for name in ('sdkconfig.defaults', 'sdkconfig.defaults.esp32s3'):
        for line in (firmware / name).read_text().splitlines():
            match = re.match(r'(CONFIG_\w+)=(.*)$', line.strip())
            if match:
                values[match[1]] = match[2]
    return values


def read_generated(path):
    values = {}
    for line in Path(path).read_text().splitlines():
        match = re.match(r'(CONFIG_\w+)=(.*)$', line)
        if match:
            values[match[1]] = match[2]
        else:
            match = re.match(r'# (CONFIG_\w+) is not set', line)
            if match:
                values[match[1]] = 'n'
    return values


defaults = read_defaults()

# Radio and memory values chosen by the RAM diet (round 2). Each is set explicitly: with
# SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y the IDF defaults would otherwise raise static RX and the BA window to 16.
EXPECTED = {
    'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM': '6',
    'CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM': '6',
    'CONFIG_ESP_WIFI_RX_BA_WIN': '12',
    'CONFIG_LWIP_IPV6': 'n',
    'CONFIG_LWIP_TCP_SND_BUF_DEFAULT': '11520',
    'CONFIG_LWIP_TCP_WND_DEFAULT': '11520',
    'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL': '1024',
    # Wi-Fi code and RX path run from flash/PSRAM instead of IRAM-placed internal RAM (round 3): 18,176 B of internal
    # RAM at run time for 4.5 percent of bulk throughput (1,295 to 1,239 kbit/s); voice needs about 64 kbit/s.
    'CONFIG_ESP_WIFI_IRAM_OPT': 'n',
    'CONFIG_ESP_WIFI_RX_IRAM_OPT': 'n',
}
for symbol, value in EXPECTED.items():
    assert defaults.get(symbol) == value, f'{symbol}: defaults say {defaults.get(symbol)!r}, expected {value!r}'

# The wifi_init.c compile-time checks (esp_wifi/src/wifi_init.c:51-57): the BA window may not exceed the
# dynamic RX count (IDF default 32, not overridden here) or twice the static RX count.
assert 'CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM' not in defaults
ba_win = int(defaults['CONFIG_ESP_WIFI_RX_BA_WIN'])
assert ba_win <= 32 and ba_win <= 2 * int(defaults['CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM'])

# Voice is ADPCM over the wireless path, not TCP PCM: the old "continuous voice PCM streaming" reason is gone.
text = (firmware / 'sdkconfig.defaults.esp32s3').read_text()
assert 'voice PCM streaming' not in text and '32KB/s' not in text, 'stale LWIP comment'

generated = os.environ.get('SDKCONFIG') or (str(firmware / 'sdkconfig') if (firmware / 'sdkconfig').exists() else None)
if generated:
    config = read_generated(generated)
    for symbol, value in defaults.items():
        assert config.get(symbol, 'n') == value.strip('"') or config.get(symbol) == value, \
            f'{symbol}: defaults {value!r}, generated config {config.get(symbol)!r}'
    checked = f'generated config {Path(generated).name} agrees with {len(defaults)} default symbols'
else:
    checked = 'no generated sdkconfig found (set SDKCONFIG=path to check one)'
print(f'sdkconfig defaults: {len(EXPECTED)} radio/memory symbols explicit and consistent; {checked} PASS')

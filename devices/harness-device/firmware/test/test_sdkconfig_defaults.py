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
    # Wi-Fi mode (W11): the values the W0 and bench6 runs measured, so the radio code in the single image behaves as it did there. The lwIP
    # windows above, the Wi-Fi buffers in PSRAM-first allocation, and the TLS record buffers (16,384 in, 4,096 out, in PSRAM; not dynamic).
    'CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP': 'y',
    'CONFIG_LWIP_TCP_RECVMBOX_SIZE': '16',
    'CONFIG_LWIP_MAX_SOCKETS': '16',
    'CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC': 'y',
    'CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN': 'y',
    'CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN': '16384',
    'CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN': '4096',
    # The PHY writes its calibration data to NVS on the first enable of a boot; the Wi-Fi bring-up therefore stays on an internal stack.
    'CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE': 'y',
    # Main task stack (plan C15): measured peak 2,092 B (build-SLP, 2026-10-28), so max(3072, peak + 1536) rounded up to 512.
    # app_main ends in vTaskDelete(NULL), so this is a boot-time cost only.
    'CONFIG_ESP_MAIN_TASK_STACK_SIZE': '4096',
}
for symbol, value in EXPECTED.items():
    assert defaults.get(symbol) == value, f'{symbol}: defaults say {defaults.get(symbol)!r}, expected {value!r}'

# The TLS-PSK link (cable_xport_tls.c, task B2): mbedTLS client-side ECDHE-PSK, set directly (esp-tls is not used, and its PSK switch would also
# select RSA-PSK). Plain PSK, DHE-PSK and RSA-PSK are off; TLS 1.3 stays off (the wrapper pins TLS 1.2 in code as well).
PSK_EXPECTED = {
    'CONFIG_MBEDTLS_PSK_MODES': 'y',
    'CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_PSK': 'y',
    'CONFIG_MBEDTLS_KEY_EXCHANGE_PSK': 'n',
    'CONFIG_MBEDTLS_KEY_EXCHANGE_DHE_PSK': 'n',
    'CONFIG_MBEDTLS_KEY_EXCHANGE_RSA_PSK': 'n',
    'CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS': 'n',
}
for symbol, value in PSK_EXPECTED.items():
    assert defaults.get(symbol) == value, f"{symbol}: defaults say {defaults.get(symbol)!r}, expected {value!r}"
assert defaults.get('CONFIG_MBEDTLS_SSL_PROTO_TLS1_3', 'n') == 'n', 'TLS 1.3 stays off'
assert 'CONFIG_ESP_TLS_PSK_VERIFICATION' not in defaults, 'the esp-tls PSK switch would select RSA-PSK; the wrapper does not use esp-tls'

assert 'CONFIG_MBEDTLS_DYNAMIC_BUFFER' not in defaults or defaults['CONFIG_MBEDTLS_DYNAMIC_BUFFER'] == 'n', 'the measured TLS numbers are for static record buffers'
assert 'CONFIG_ESP_WIFI_NVS_ENABLED' not in defaults, 'the driver keeps its NVS option as measured; wifi_sta.c switches it off at run time (nvs_enable = 0, WIFI_STORAGE_RAM)'
# Wi-Fi mode never turns the radio on in USB mode: the radio code is linked in, but its only entry is net_link_start() (test_net_link_source.py).

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
    for symbol, value in PSK_EXPECTED.items():
        assert config.get(symbol, "n") == value, f"{symbol}: generated config {config.get(symbol)!r}, expected {value!r}"
    assert config.get('CONFIG_MBEDTLS_SSL_PROTO_TLS1_3', 'n') != 'y', 'TLS 1.3 is off in the generated config'
    checked = f'generated config {Path(generated).name} agrees with {len(defaults)} default symbols'
else:
    checked = 'no generated sdkconfig found (set SDKCONFIG=path to check one)'

# sdkconfig.defaults.ble: the BLE settings proven on the profile images (P2, P2f), kept OUT of the default list. IDF loads
# sdkconfig.defaults and sdkconfig.defaults.<target> by itself; a file with another suffix is read only when it is named in
# -DSDKCONFIG_DEFAULTS, so the product (no Bluetooth) is unchanged.
ble_text = (firmware / 'sdkconfig.defaults.ble').read_text()
ble = dict(re.findall(r'^(CONFIG_\w+)=(.*?)\s*$', ble_text, re.M))
BLE_EXPECTED = {
    'CONFIG_BT_ENABLED': 'y', 'CONFIG_BT_NIMBLE_ENABLED': 'y', 'CONFIG_BT_BLUEDROID_ENABLED': 'n',
    'CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM': '1',
    'CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL': 'y',          # host pools in PSRAM
    'CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT': 'n',               # with this off the dial must not request the 2M PHY
    'CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY': 'y',                  # plan C12: +20,288 B internal RAM at run time
}
for symbol, value in BLE_EXPECTED.items():
    assert ble.get(symbol) == value, f'{symbol}: sdkconfig.defaults.ble says {ble.get(symbol)!r}, expected {value!r}'
assert not (set(ble) & set(defaults)) or all(ble[s] == defaults[s] for s in set(ble) & set(defaults)), 'no conflict with the defaults'
assert not [s for s in ble if 'BENCH' in s or s.startswith('CONFIG_FREERTOS_')], 'nothing bench-only'
assert not any('BT_' in s for s in defaults), 'the default files stay Bluetooth-free'
assert 'sdkconfig.defaults.ble' not in (firmware / 'CMakeLists.txt').read_text().split('file(GLOB')[0], 'not added to the load list'
ble_checked = 'no BLE build config given (set SDKCONFIG_BLE=path to a build made with the BLE defaults list)'
ble_build = os.environ.get('SDKCONFIG_BLE')
if ble_build:
    config = read_generated(ble_build)
    for symbol, value in {**defaults, **ble}.items():
        assert config.get(symbol, 'n') == value.strip('"') or config.get(symbol) == value, f'{symbol}: expected {value!r}, BLE build config {config.get(symbol)!r}'
    ble_checked = f'BLE build config {Path(ble_build).name} agrees with {len(ble)} BLE symbols'
print(f'sdkconfig defaults: {len(EXPECTED)} radio/memory symbols explicit and consistent; {checked}; '
      f'sdkconfig.defaults.ble has {len(ble)} symbols, not in the default list; {ble_checked} PASS')

"""The battery sleep overlay (sdkconfig.defaults.sleep) and the DEVICE_BATTERY_SLEEP build flag.

The overlay is NOT loaded by default (like sdkconfig.defaults.ble), so a default image has no power management at all and the
flag-off image is the image it was. Checked here:
  * every CONFIG_ name the overlay sets exists in an ESP-IDF Kconfig file (when IDF_PATH is set; this catches a typo, which
    the build would silently drop),
  * neither default file that IDF loads on its own (sdkconfig.defaults, sdkconfig.defaults.esp32s3) names the overlay's options,
    and nothing in the top CMakeLists loads the overlay,
  * the flag is wired in main/CMakeLists.txt: sources only under the flag, a configure error without the overlay's options,
    esp_pm only under the flag,
  * power_sleep.c is only compiled with the flag and refuses to compile without the options,
  * the policy source has no IDF include (host tests compile it with a plain cc).
"""
from pathlib import Path
import os
import re

fw = Path(__file__).resolve().parent / '..'
overlay = (fw / 'sdkconfig.defaults.sleep').read_text()
settings = dict(re.findall(r'^(CONFIG_[A-Z0-9_]+)=(\S+)', overlay, re.M))
assert settings, 'no CONFIG_ lines in the overlay'

# The options the plan names (section 3.2) are all there, with the values the build depends on.
for name, value in {'CONFIG_PM_ENABLE': 'y', 'CONFIG_FREERTOS_USE_TICKLESS_IDLE': 'y',
                    'CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION': 'y', 'CONFIG_PM_LIGHT_SLEEP_CALLBACKS': 'y',
                    'CONFIG_SPIRAM_MEMTEST': 'n'}.items():
    assert settings.get(name) == value, (name, settings.get(name))
# Left alone: the tick rate and the frequency are the defaults' business.
for name in ('CONFIG_FREERTOS_HZ', 'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240', 'CONFIG_PM_DFS_INIT_AUTO'):
    assert name not in settings, f'{name} must stay as sdkconfig.defaults has it'

# Typo guard: each symbol is declared in some IDF Kconfig file.
idf = os.environ.get('IDF_PATH')
if idf and (Path(idf) / 'components').is_dir():
    declared = set()
    for kc in (Path(idf) / 'components').rglob('Kconfig*'):
        if kc.is_file():
            declared.update(re.findall(r'^\s*(?:menu)?config\s+([A-Z0-9_]+)', kc.read_text(errors='replace'), re.M))
    missing = [n for n in settings if n[len('CONFIG_'):] not in declared]
    assert not missing, f'not an IDF 5.5 Kconfig symbol: {missing}'
    checked = f'{len(settings)} names found in the IDF Kconfig files'
else:
    checked = 'IDF_PATH not set: Kconfig name check skipped'

# Not loaded on its own: IDF reads sdkconfig.defaults and sdkconfig.defaults.<target> only.
loaded = [(fw / 'sdkconfig.defaults').read_text(), (fw / 'sdkconfig.defaults.esp32s3').read_text()]
for text in loaded:
    for name in ('CONFIG_PM_ENABLE', 'CONFIG_FREERTOS_USE_TICKLESS_IDLE', 'CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION',
                 'CONFIG_PM_SLP_IRAM_OPT', 'CONFIG_PM_LIGHT_SLEEP_CALLBACKS'):
        assert not re.search(r'^' + name + r'=', text, re.M), f'{name} leaked into a default file'
assert 'sdkconfig.defaults.sleep' not in (fw / 'CMakeLists.txt').read_text().replace('sdkconfig.defaults.*', ''), \
    'the top CMakeLists must not load the overlay'
assert not (fw / 'sdkconfig.defaults.esp32s3.sleep').exists()

# The flag, in main/CMakeLists.txt.
cmake = (fw / 'main/CMakeLists.txt').read_text()
block = re.search(r'if\(DEVICE_BATTERY_SLEEP\)\n    if\(NOT CONFIG_PM_ENABLE.*?\nendif\(\)\n', cmake, re.S)
assert block, 'DEVICE_BATTERY_SLEEP block'
assert 'FATAL_ERROR' in block[0] and 'sdkconfig.defaults.sleep' in block[0]
assert 'DEVICE_BATTERY_SLEEP=1' in block[0]
assert re.search(r'target_sources\(\$\{COMPONENT_LIB\} PRIVATE "power_policy\.c" "power_sleep\.c"\)', block[0])
for src in ('power_policy.c', 'power_sleep.c'):
    assert cmake.count(f'"{src}"') == 1, f'{src} must be listed once, inside the flag block only'
assert re.search(r'if\(DEVICE_BATTERY_SLEEP\)\n    list\(APPEND BOARD_PRIV_REQUIRES esp_pm\)', cmake)
assert 'PRIV_REQUIRES ${BOARD_PRIV_REQUIRES}' in cmake
# The default is OFF: nothing sets the flag.
assert not re.search(r'set\(DEVICE_BATTERY_SLEEP\b', cmake)

# The glue refuses to compile without the options; the pure policy has no IDF include.
glue = (fw / 'main/power_sleep.c').read_text()
assert re.search(r'#if !CONFIG_PM_ENABLE \|\| !CONFIG_FREERTOS_USE_TICKLESS_IDLE\n#error', glue)
header = (fw / 'main/power_sleep.h').read_text()
assert '#ifdef DEVICE_BATTERY_SLEEP' in header and 'static inline void power_sleep_init(void) {}' in header
for f in ('power_policy.c', 'power_policy.h'):
    text = (fw / 'main' / f).read_text()
    includes = re.findall(r'#include\s+[<"]([^>"]+)[>"]', text)
    assert all(i in ('power_policy.h', 'stdbool.h', 'stdint.h', 'string.h') for i in includes), (f, includes)
# app_main calls the init once, right after the board is known and before anything is initialised.
app = (fw / 'main/app_main.c').read_text()
assert app.count('power_sleep_init();') == 1
assert app.index('board_detect();      //') < app.index('power_sleep_init();') < app.index('config_store_init();')

print(f'sleep overlay: {checked}; not loaded by default; flag wired in CMake, default OFF, glue guarded PASS')

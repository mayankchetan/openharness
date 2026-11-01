"""B2: build the TLS wrapper (main/cable_xport_tls.c) against the IDF copy of mbedTLS on the host and run test_cable_xport_tls.c.

The mbedTLS sources are not stubbed: every file under $IDF_PATH/components/mbedtls/mbedtls/library is compiled with
host_tls/mbedtls_host_config.h (a minimal config: TLS 1.2, ECDHE-PSK, AES-CBC, SHA-2; plus plain PSK, DHE-PSK, GCM and SHA-1
so the test peer can offer suites the dial must refuse). The wrapper's FreeRTOS, esp_log and esp_timer names are host
shims in host_tls/ (pthread mutex, a log hook). The wrapper and the test run under the address and undefined-behaviour
sanitizers; set TSAN=1 to build them under the thread sanitizer instead (the concurrency check).

Needs IDF_PATH (run.sh calls this inside its IDF block) and a C compiler. About 20 s.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
main = here / '../main'
idf = os.environ.get('IDF_PATH')
assert idf, 'IDF_PATH is not set: the host build of mbedTLS needs components/mbedtls/mbedtls'
mbed = Path(idf) / 'components/mbedtls/mbedtls'
assert (mbed / 'library/ssl_tls.c').exists(), f'no mbedTLS sources under {mbed}'
tsan = os.environ.get('TSAN') == '1'
cc = os.environ.get('CC', 'cc')
# PRODUCT_ONLY=1 builds mbedTLS with the ECDHE-PSK key exchange alone, as sdkconfig.defaults.esp32s3 does (the refusal cases then
# find no shared suite instead of a suite the dial declined)
product_only = os.environ.get('PRODUCT_ONLY') == '1'

with tempfile.TemporaryDirectory() as tmp:
    out = Path(tmp)
    base = [cc, '-std=c11', '-g', '-w', f'-I{here / "host_tls"}', f'-I{mbed / "include"}', f'-I{mbed / "library"}',
            '-DMBEDTLS_CONFIG_FILE="mbedtls_host_config.h"', *(['-DHOST_PRODUCT_SUITES_ONLY'] if product_only else [])]

    def build(source: Path):
        obj = out / (source.stem + '.o')
        # under TSAN=1 the mbedTLS objects are instrumented too, so a shared ssl_context would be reported
        done = subprocess.run(base + ['-O1', *(['-fsanitize=thread'] if tsan else []), '-c', str(source), '-o', str(obj)],
                              capture_output=True, text=True)
        assert done.returncode == 0, f'{source.name}: {done.stderr[:2000]}'
        return str(obj)

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        objects = list(pool.map(build, sorted((mbed / 'library').glob('*.c'))))

    sanitize = ['-fsanitize=thread'] if tsan else ['-fsanitize=address,undefined']
    strict = ['-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-pthread', *sanitize,
              f'-I{here / "host_tls"}', f'-I{main}', f'-I{mbed / "include"}', '-DMBEDTLS_CONFIG_FILE="mbedtls_host_config.h"', *(['-DHOST_PRODUCT_SUITES_ONLY'] if product_only else [])]
    exe = out / 'test_cable_xport_tls'
    link = subprocess.run([cc, *strict, '-Wl,--wrap=mbedtls_ssl_write', '-Wl,--wrap=mbedtls_ssl_read', '-Wl,--wrap=mbedtls_ssl_free', '-o', str(exe), str(here / 'test_cable_xport_tls.c'),
                           str(main / 'cable_xport_tls.c'), str(main / 'cable_frame.c'), *objects, '-lm'],
                          capture_output=True, text=True)
    assert link.returncode == 0, f'build failed:\n{link.stderr[:4000]}'
    if len(sys.argv) == 3 and sys.argv[1] == '--build-only':
        # for the Node interop spec (cli/src/cable/tlsPsk.firmware.spec.ts): the same binary has a --connect mode
        shutil.copy(exe, sys.argv[2])
        print(f'built {sys.argv[2]}')
        sys.exit(0)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', TSAN_OPTIONS='halt_on_error=1')
    # TSan needs a fixed address-space layout on some kernels (WSL2): setarch -R switches ASLR off for the run.
    prefix = ['setarch', os.uname().machine, '-R'] if tsan else []
    done = subprocess.run([*prefix, str(exe)], capture_output=True, text=True, timeout=240, env=env)
    sys.stdout.write(done.stdout)
    sys.stderr.write(done.stderr)
    assert done.returncode == 0, f'test_cable_xport_tls exited {done.returncode}'
    assert re.search(r'cable_xport_tls: \d+ checks PASS', done.stdout)
    print(f'test_cable_xport_tls ({"thread" if tsan else "address,undefined"} sanitizer): PASS')

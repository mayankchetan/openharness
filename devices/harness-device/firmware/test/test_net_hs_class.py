"""W11: build test_net_hs_class.c against the IDF copy of the mbedTLS headers and run it (needs IDF_PATH, so run.sh calls it in its IDF block)."""
from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
idf = os.environ.get('IDF_PATH')
assert idf, 'IDF_PATH is not set: the test needs components/mbedtls/mbedtls/include'
mbed = Path(idf) / 'components/mbedtls/mbedtls/include'
assert (mbed / 'mbedtls/ssl.h').exists(), mbed

with tempfile.TemporaryDirectory(prefix='harness-hs-class-') as folder:
    exe = Path(folder) / 'test_net_hs_class'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', f'-I{here / "host_tls"}', f'-I{here / "../main"}', f'-I{mbed}',
                    '-DMBEDTLS_CONFIG_FILE="mbedtls_host_config.h"', str(here / 'test_net_hs_class.c'), '-o', str(exe)], check=True)
    done = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    print(done.stdout.strip())

"""Compile main/adpcm_ima.c with the host test and run it against the shared vectors (task W2)."""
from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='harness-adpcm-') as directory:
    exe = Path(directory) / 'test_adpcm_ima'
    flags = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
             '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
             '-I', str(here / '../main')]
    subprocess.run(flags + [str(here / 'test_adpcm_ima.c'), str(here / '../main/adpcm_ima.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(here / 'vectors/adpcm_ima.txt')], check=True)

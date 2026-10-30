"""Compile and run test_vox_gate.c: the safety gates of the VOX probe in review mode (main/vox_gate.h, pure C)."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='harness-vox-gate-') as directory:
    exe = Path(directory) / 'test_vox_gate'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-I', str(here / '../main'),
                    str(here / 'test_vox_gate.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

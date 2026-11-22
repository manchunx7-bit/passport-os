"""Compile and run the real radio catalog with deterministic HTTP/DNS fixtures."""
import os
import subprocess
import tempfile
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CJSON = ROOT / 'tests/vendor/cjson'
with tempfile.TemporaryDirectory(prefix='passport-radio-') as tmp:
    obj = Path(tmp) / 'json.o'
    exe = Path(tmp) / 'radio'
    subprocess.run(['cc', '-c', '-fsanitize=address,undefined', '-I'+str(CJSON),
                    str(CJSON / 'cJSON.c'), '-o', str(obj)], check=True)
    # ESP_LOG mocks compile the log tag away; production builds still use it.
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-const-variable',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-Itests/radio_mocks', '-Imain', '-I'+str(CJSON),
                    'tests/test_radio_catalog.cc', 'main/apps/radio/radio_catalog.cc',
                    str(obj), '-o', str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)]+sys.argv[1:], check=True)

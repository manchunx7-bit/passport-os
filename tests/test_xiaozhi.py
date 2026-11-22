"""Compile production Xiaozhi sources with deterministic fault-injection mocks."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="passport-xiaozhi-") as temporary:
    cjson = Path(temporary) / "cjson.o"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Itests/vendor/cjson",
                    "-c", "tests/vendor/cjson/cJSON.c", "-o", str(cjson)],
                   cwd=ROOT, check=True)
    for name in ("app", "audio", "protocol"):
        executable = Path(temporary) / name
        command = [os.environ.get("CXX", "c++"), "-std=c++20", "-g", "-O1",
                   "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                   "-Wno-unused-variable", "-Wno-misleading-indentation",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-Itests/xiaozhi_mocks", "-Itests/vendor/cjson",
                   "-Imain", "-Icomponents/bsp/include",
                   f"tests/test_xiaozhi_{name}.cc",
                   "-o", str(executable)]
        if name == "protocol":
            command.append(str(cjson))
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)
print("Xiaozhi host regression: PASS (device tests not run)")

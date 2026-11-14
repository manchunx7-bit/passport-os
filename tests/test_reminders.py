"""Production reminder service and MCP with deterministic storage failure injection."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="passport-reminders-") as tmp:
    target = str(Path(tmp) / "reminders")
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-Wno-misleading-indentation", "-g", "-fsanitize=address,undefined",
                    "-Itests/reminder_mocks", "-Itests/vendor/cjson",
                    "-Imain", "-Icomponents/bsp/include", "tests/test_reminders.cc",
                    "tests/vendor/cjson/cJSON.c", "-o", target], cwd=root, check=True)
    subprocess.run([target], cwd=root, check=True)

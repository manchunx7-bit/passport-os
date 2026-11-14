from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='passport-lists-') as tmp:
    target=str(Path(tmp)/'lists')
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
        '-g','-fsanitize=address,undefined','-Itests/reminder_mocks','-Itests/vendor/cjson','-Imain',
        'tests/test_lists.cc','tests/vendor/cjson/cJSON.c','-o',target],cwd=root,check=True)
    subprocess.run([target],cwd=root,check=True)

"""Check the build script stays portable and the safe-flashing docs stay complete."""
from pathlib import Path
import os
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = (ROOT / 'build.sh').read_text(encoding='utf-8')


def make_idf(directory, build_log, activation_log=None):
    directory.mkdir(parents=True)
    activation = '' if activation_log is None else f'echo activated >>"{activation_log}"\n'
    (directory / 'export.sh').write_text(
        f'#!/bin/sh\nexport IDF_PATH="{directory}"\n{activation}', encoding='utf-8')
    tools = directory / 'tools'
    tools.mkdir()
    idf_py = tools / 'idf.py'
    idf_py.write_text(
        '#!/bin/sh\n'
        'if [ "$1" = "--version" ]; then echo "ESP-IDF v5.5.3"; exit 0; fi\n'
        f'printf "%s\\n" "$@" > "{build_log}"\n',
        encoding='utf-8')
    idf_py.chmod(idf_py.stat().st_mode | stat.S_IXUSR)
    return directory


def make_project(directory):
    directory.mkdir(parents=True)
    (directory / 'build.sh').write_text(BUILD, encoding='utf-8')
    (directory / 'sdkconfig.defaults').write_text('# test default\n', encoding='utf-8')
    (directory / 'sdkconfig.diag').write_text('# test diag\n', encoding='utf-8')
    return directory / 'build.sh'


def run(script, **environment):
    base = {key: value for key, value in os.environ.items()
            if key not in ('IDF_DIR', 'IDF_PATH')}
    base.update(environment)
    return subprocess.run(['bash', str(script)], env=base,
                          capture_output=True, text=True)


# GNU-only flags break the BSD cp/cat shipped with macOS.
assert 'cp --' not in BUILD and 'cat --' not in BUILD
assert 'IDF_DIR' in BUILD and 'IDF_PATH' in BUILD
assert '.espressif/v5.5.3/esp-idf' in BUILD
assert 'activate_idf_' in BUILD and 'export.sh' in BUILD
syntax = subprocess.run(['bash', '-n', str(ROOT / 'build.sh')], capture_output=True, text=True)
assert syntax.returncode == 0, syntax.stderr

with tempfile.TemporaryDirectory(prefix='passport-build-') as tmp:
    tmp = Path(tmp)
    home = tmp / 'home'
    home.mkdir()

    # macOS EIM install: activate_idf_<version>.sh -e is preferred over export.sh.
    eim_log = tmp / 'eim-idf.log'
    eim = make_idf(home / '.espressif/v5.5.3/esp-idf', eim_log)
    activate = home / '.espressif/tools/activate_idf_v5.5.3.sh'
    activate.parent.mkdir(parents=True)
    activate.write_text(
        '#!/bin/sh\n'
        'printf "PATH=%s\\n" "' + str(home / 'tools with space') + '"\n'
        'printf "SYSTEM_PATH=/usr/bin:/bin\\n"\n'
        f'printf "IDF_PATH=%s\\n" "{eim}"\n'
        'printf "IDF_VERSION=5.5.3\\n"\n', encoding='utf-8')
    activate.chmod(activate.stat().st_mode | stat.S_IXUSR)

    script = make_project(tmp / 'project')
    result = run(script, HOME=str(home))
    assert result.returncode == 0, result.stdout + result.stderr
    assert eim_log.read_text(encoding='utf-8').splitlines()[:3] == [
        '-B', 'build', f'-DSDKCONFIG={tmp / "project/build/sdkconfig"}'], result.stdout
    assert 'v5.5.3' in result.stdout

    # IDF_PATH is honored when IDF_DIR is absent.
    other_log = tmp / 'other-idf.log'
    other_activation = tmp / 'other-activate.log'
    other = make_idf(tmp / 'other-idf', other_log, other_activation)
    result = run(script, HOME=str(tmp / 'empty-home'), IDF_PATH=str(other))
    assert result.returncode == 0, result.stdout + result.stderr
    assert other_activation.read_text(encoding='utf-8').strip() == 'activated'
    assert other_log.exists()

    # Explicit IDF_DIR wins over IDF_PATH.
    other_log.unlink()
    result = run(script, HOME=str(home), IDF_DIR=str(eim), IDF_PATH=str(other))
    assert result.returncode == 0, result.stdout + result.stderr
    assert eim_log.exists() and not other_log.exists()

    result = run(script, HOME=str(tmp / 'empty-home'))
    assert result.returncode == 1
    assert 'IDF_DIR' in result.stdout and 'IDF_PATH' in result.stdout

zh = (ROOT / 'docs/safe-flashing.zh_CN.md').read_text(encoding='utf-8')
en = (ROOT / 'docs/safe-flashing.en.md').read_text(encoding='utf-8')
for text in (zh, en):
    for marker in ('0x10000', '0x0', 'erase-flash', 'write_flash', 'app-flash',
                   'macOS', 'Linux', 'Windows', 'Invalid image block'):
        assert marker in text, marker
assert 'safe-flashing.zh_CN.md' in (ROOT / 'README.md').read_text(encoding='utf-8')
assert 'safe-flashing.en.md' in (ROOT / 'docs/README.en.md').read_text(encoding='utf-8')
assert 'safe-flashing.zh_CN.md' in (ROOT / 'docs/MAINTENANCE.md').read_text(encoding='utf-8')
assert 'safe-flashing' in (ROOT / '.github/CONTRIBUTING.md').read_text(encoding='utf-8')
assert 'safe-flashing' in (ROOT / '.github/CONTRIBUTING.zh_CN.md').read_text(encoding='utf-8')

print('Build script and flashing docs: PASS')

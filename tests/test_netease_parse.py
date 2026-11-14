"""Compare the device's streaming NetEase JSON scanner with Python's json module
on real public API responses (tests/fixtures/netease), plus scanner/LRC edge cases."""
from pathlib import Path
import json
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
fixtures = root / 'tests/fixtures/netease'


def expected(mode):
    if mode == 'playlists':
        data = json.loads((fixtures / 'user_playlist.json').read_text(encoding='utf-8'))
        return [f"{p['id']}\t{p['name']}\t{p['trackCount']}\t{p['userId']}" for p in data['playlist']]
    if mode == 'trackids':
        data = json.loads((fixtures / 'playlist_detail.json').read_text(encoding='utf-8'))
        return [f"{t['id']}\t\t\t" for t in data['playlist']['trackIds']]
    data = json.loads((fixtures / 'song_detail.json').read_text(encoding='utf-8'))
    return [f"{s['id']}\t{s['name']}\t{s['ar'][0]['name'] if s['ar'] else ''}\t{s['dt']}" for s in data['songs']]


with tempfile.TemporaryDirectory(prefix='passport-netease-') as temporary:
    exe = Path(temporary) / 'parse'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g',
                    '-Imain', 'tests/test_netease_parse.cc', 'main/apps/netease/netease_json.cc',
                    'main/apps/netease/netease_lyrics.cc', '-o', str(exe)], cwd=root, check=True)
    subprocess.run([str(exe), 'self'], check=True)
    for mode, name in [('playlists', 'user_playlist.json'), ('trackids', 'playlist_detail.json'),
                       ('songs', 'song_detail.json')]:
        out = subprocess.run([str(exe), mode, str(fixtures / name)], check=True, capture_output=True)
        got = out.stdout.decode('utf-8').splitlines()
        want = expected(mode)
        assert got == want, f'{mode}: {len(got)} vs {len(want)} records\n{got[:3]}\n{want[:3]}'
        print(f'NetEase {mode}: {len(got)} records match json module')
print('NetEase streaming parse: PASS')

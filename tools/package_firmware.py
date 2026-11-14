#!/usr/bin/env python3
"""Create a local firmware candidate with an exact working-tree source snapshot."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = {'.github', 'bootloader_components', 'components', 'docs', 'main', 'tests', 'third_party', 'tools', 'web'}
SOURCE_ROOT_FILES = {'.gitignore', 'CMakeLists.txt', 'build.sh', 'dependencies.lock',
                     'partitions.csv', 'sdkconfig.defaults', 'LICENSE', 'README.md', 'README.zh_CN.md', 'DESIGN.md', 'PRODUCT.md', 'AGENTS.md', 'sdkconfig.diag'}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_zip(path: Path, entries: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(entries.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100755 if name.endswith('.sh') else 0o100644) << 16
            archive.writestr(info, data)


def source_snapshot() -> dict[str, bytes]:
    names = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'],
                                    cwd=ROOT).decode('utf-8').split('\0')
    result = {}
    for name in set(names) | SOURCE_ROOT_FILES:
        path = Path(name)
        if name not in SOURCE_ROOT_FILES and (not path.parts or path.parts[0] not in SOURCE_DIRS):
            continue
        if any(part in ('__pycache__', '.pytest_cache', '.cache', '.git') for part in path.parts):
            continue
        if path.suffix.lower() in ('.bin', '.elf', '.map', '.log', '.pyc') or '.env' in path.name:
            continue
        full_path = ROOT / path
        if full_path.is_file():
            if full_path.is_symlink() or not full_path.resolve().is_relative_to(ROOT):
                raise ValueError('source snapshot cannot follow external links: '+name)
            result[name] = full_path.read_bytes()
    for name in SOURCE_ROOT_FILES:
        if name not in result:
            raise ValueError('required source file is missing: '+name)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT/'build-public')
    parser.add_argument('--date', required=True, help='release date in the maintainer timezone, YYYY-MM-DD')
    parser.add_argument('--verification', type=Path, required=True, help='public-safe verification JSON for this version')
    args = parser.parse_args()
    if not re.fullmatch(r'\d{4}-\d{2}-\d{2}', args.date):
        parser.error('--date must use YYYY-MM-DD')
    build = args.build_dir.resolve()
    subprocess.run([sys.executable, str(ROOT/'tools/verify_firmware.py'), str(build), '--public'], check=True)
    app = (build/'passport-os.bin').read_bytes()
    full = (build/'FoloToy-AI-Passport-full.bin').read_bytes()
    version = app[0x30:0x50].split(b'\0')[0].decode('ascii')
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+)?', version):
        raise ValueError('invalid firmware version')
    verification = json.loads(args.verification.read_text(encoding='utf-8'))
    if verification.get('version') != version:
        raise ValueError('verification version differs from firmware version')
    if any(value != 0xff for value in full[0x9000:0x10000]):
        raise ValueError('public full image must not contain device configuration')
    source = source_snapshot()
    if f'set(PROJECT_VER "{version}")' not in source['CMakeLists.txt'].decode('utf-8'):
        raise ValueError('source version differs from firmware version')
    for pattern in (rb'mgst_[A-Za-z0-9_-]{12,}', rb'(?:ghp_|github_pat_)[A-Za-z0-9_]{20,}',
                    rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----[\r\n]+[A-Za-z0-9+/=\r\n]{64,}-----END'):
        if re.search(pattern, full):
            raise ValueError('possible personal token in firmware')
    release = ROOT/'release'
    release.mkdir(exist_ok=True)
    target = release/('website-firmware-'+version)
    bundle = release/('Passport-OS-v'+version+'-candidate.zip')
    if target.exists() or bundle.exists():
        raise FileExistsError('candidate already exists; preserve it and use a new version')
    source_hashes = {name: digest(data) for name, data in sorted(source.items())}
    with tempfile.TemporaryDirectory(prefix='.package-', dir=release) as temporary:
        stage = Path(temporary)
        source_name = 'Passport-OS-v'+version+'-source.zip'
        write_zip(stage/source_name, {'passport-os/'+name: data for name, data in source.items()})
        app_name = 'FoloToy-AI-Passport-v'+version+'-app.bin'
        full_name = 'FoloToy-AI-Passport-v'+version+'-full.bin'
        (stage/app_name).write_bytes(app)
        (stage/full_name).write_bytes(full)
        (stage/'source-files.sha256.json').write_text(json.dumps(source_hashes, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
        notes = ROOT/('docs/release-rc'+version.split('rc.')[-1]+'.md')
        if not notes.is_file():
            raise ValueError('candidate release notes are missing: '+str(notes))
        (stage/'RELEASE-NOTES.md').write_bytes(notes.read_bytes())
        (stage/'verification.json').write_text(json.dumps(verification, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
        files = {path.name: dict(bytes=path.stat().st_size, sha256=digest(path.read_bytes()))
                 for path in stage.iterdir()}
        manifest = dict(version=version, date=args.date, status='local_candidate_not_published',
                        chip='esp32c3', flash_bytes=8388608, esp_idf='5.5.3',
                        images=[dict(file=app_name, offset='0x10000', **files[app_name]),
                                dict(file=full_name, offset='0x0', **files[full_name])],
                        source=dict(reference_head=(subprocess.run(['git', 'rev-parse', '--verify', 'HEAD'], cwd=ROOT, capture_output=True, text=True).stdout.strip() or None),
                                    dirty=bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT)),
                                    authoritative_snapshot=source_name, file_count=len(source),
                                    note='Source ZIP and per-file hashes identify this candidate; reference HEAD alone does not.'),
                        files=files, public_defaults_verified=True,
                        app_partition_remaining=0x400000-len(app),
                        published=False, private_device_data_included=False)
        (stage/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
        checksums = ''.join(digest(path.read_bytes())+'  '+path.name+'\n'
                            for path in sorted(stage.iterdir()))
        (stage/'SHA256.txt').write_text(checksums, encoding='ascii')
        if source_snapshot() != source:
            raise ValueError('source changed during packaging')
        write_zip(bundle, {target.name+'/'+path.name: path.read_bytes() for path in stage.iterdir()})
        target.mkdir()
        for path in stage.iterdir():
            path.rename(target/path.name)
    print('Local candidate: '+str(bundle))
    print('Candidate ZIP SHA256: '+digest(bundle.read_bytes()))
    print('Source snapshot files: '+str(len(source)))


if __name__ == '__main__':
    main()

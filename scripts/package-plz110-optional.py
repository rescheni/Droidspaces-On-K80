#!/usr/bin/env python3
"""Package the exact PLZ110 PID/IPC and devtmpfs binaries validated for v0.1.1."""
import argparse
import hashlib
import json
import re
import runpy
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'build/plz110-0.1.1.json'
ARM64 = runpy.run_path(str(ROOT / 'scripts/package-module.py'))['arm64']


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def package(output):
    profile = json.loads(PROFILE.read_text())
    for name, expected in profile['sources'].items():
        require(sha((ROOT / name).read_bytes()) == expected, f'source changed: {name}')
    inputs = {}
    for name, item in profile['inputs'].items():
        data = (ROOT / item['path']).read_bytes()
        require(sha(data) == item['sha256'], f'validated input changed: {name}')
        inputs[name] = data
    for name in ('namespace', 'devtmpfs'):
        ARM64(inputs[name])
    for name in ('gki_console', 'plz110_console'):
        log = inputs[name].decode()
        require('DS_DEVTMPFS_VM_RESULT=0' in log and 'DS_DEVTMPFS_PASS:' in log,
                f'incomplete VM validation: {name}')
        require(not re.search(r'BUG:|WARNING:|Oops|Kernel panic', log), f'failed VM: {name}')
    require('DS_DEVTMPFS_PASS:' in inputs['device_console'].decode(), 'missing device test')
    validation = json.loads(inputs['validation'])
    require(validation['module_sha256'] == sha(inputs['devtmpfs']), 'device module differs')
    require(validation['kernel_btf_sha256'] == sha(inputs['device_btf']), 'device BTF differs')
    device = validation['device_validation']
    require(device['result'] == 'passed' and device['ready'] and
            device['errors_before'] == device['errors_after'] == 0 and
            device['fixture_unloaded'] and device['log_sha256'] == sha(inputs['device_console']),
            'device validation incomplete')
    for key, name in (('gki', 'gki_console'), ('plz110_kernel', 'plz110_console')):
        require(validation['vm'][key]['result'] == 'passed' and
                validation['vm'][key]['console_sha256'] == sha(inputs[name]), 'VM log differs')
    # Bind the public provenance to exactly the tested image, binaries and sources.
    evidence = dict(profile=profile, validation=validation)
    evidence['validation']['device_validation'].pop('boot_id', None)
    files = {name: (ROOT / 'kernelsu-plz110' / name).read_bytes() for name in
             ('module.prop', 'common.sh', 'customize.sh', 'post-fs-data.sh', 'service.sh', 'skip_mount')}
    prop = files['module.prop'].decode().splitlines()
    require({'version=v0.1.1', 'versionCode=3', 'author=Shikochin', 'id=droidspaces_lkm'} <= set(prop),
            'unexpected package identity')
    require(profile['release'].encode() in files['common.sh'] and
            sha(inputs['device_btf']).encode() in files['common.sh'], 'loader target differs')
    files['kernel/droidspaces_ns.ko'] = inputs['namespace']
    files['kernel/droidspaces_devtmpfs.ko'] = inputs['devtmpfs']
    files['kernel/SHA256SUMS'] = ''.join(
        f'{sha(files["kernel/" + name])}  {name}\n'
        for name in ('droidspaces_ns.ko', 'droidspaces_devtmpfs.ko')).encode()
    files['LICENSE'] = (ROOT / 'LICENSE').read_bytes()
    files['README.md'] = (ROOT / 'docs/releases-v0.1.1.md').read_bytes()
    files['validation.json'] = (json.dumps(evidence, indent=2) + '\n').encode()
    files['SOURCE.txt'] = (
        'Source: https://github.com/Shikochin/Droidspaces-LKM/tree/v0.1.1\n'
        'PID/IPC: unchanged v0.1.0 payload, original source commit 2147acd.\n'
        'devtmpfs: src/devtmpfs/ and vendor/kallrecon/; provenance: src/devtmpfs/ORIGIN.\n'
        'Source and validation hashes: validation.json. No user namespace support.\n'
    ).encode()
    output.mkdir(parents=True, exist_ok=True)
    archive = output / 'Droidspaces-LKM-KernelSU-v0.1.1-PLZ110.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 10, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (0o100755 if name.endswith('.sh') else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    standalone = {'droidspaces_ns-6.12.69-canoe.ko': inputs['namespace'],
                  'droidspaces_devtmpfs-6.12.69-plz110.ko': inputs['devtmpfs']}
    for name, data in standalone.items():
        (output / name).write_bytes(data)
    artifacts = sorted([archive, *(output / name for name in standalone)])
    (output / 'SHA256SUMS').write_text(''.join(f'{sha(p.read_bytes())}  {p.name}\n' for p in artifacts))
    print('\n'.join(str(p) for p in [*artifacts, output / 'SHA256SUMS']))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'out/release-v0.1.1')
    package(parser.parse_args().output)

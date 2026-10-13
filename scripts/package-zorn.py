#!/usr/bin/env python3
"""Package the zorn / 6.1.138 device module and its validation evidence.

The profile in build/zorn-6.1.json pins every input by SHA-256: the source set,
the compiled module, the probe and the recorded device console. Packaging fails
if anything changed, so a released ZIP always describes one exact build.
"""
import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'build/zorn-6.1.json'
MODULE_FILES = ('module.prop', 'common.sh', 'customize.sh', 'post-fs-data.sh',
                'service.sh', 'skip_mount')
PROBE_PASS = ('PASS sysv-create', 'PASS sysv-message-roundtrip',
              'PASS sysv-new-ns-isolated', 'PASS mqueue-open',
              'PASS mqueue-roundtrip', 'PASS mqueue-new-ns-isolated',
              'PASS pidns-init-is-pid1')
EVIDENCE = ('device_btf', 'device-check', 'device-probe', 'device-loader')

def sha(data):
    return hashlib.sha256(data).hexdigest()
def plain(data):
    """Drop SGR colour codes and CR from captured console output."""
    return re.sub(r'\x1b\[[0-9;]*m', '', data.decode()).replace('\r', '')

def require(ok, message):
    if not ok:
        raise ValueError(message)


def collect():
    """Read every input the release depends on."""
    profile = json.loads(PROFILE.read_text())
    inputs = {name: (ROOT / item['path']).read_bytes()
              for name, item in profile['inputs'].items()}
    sources = {name: sha((ROOT / name).read_bytes()) for name in profile['sources']}
    return profile, inputs, sources


def record():
    """Refresh the pinned hashes from the current tree and build output."""
    profile = json.loads(PROFILE.read_text())
    for name, item in profile['inputs'].items():
        path = ROOT / item['path']
        require(path.is_file(), f'missing input: {name} ({item["path"]})')
        item['sha256'] = sha(path.read_bytes())
    for name in profile['sources']:
        profile['sources'][name] = sha((ROOT / name).read_bytes())
    PROFILE.write_text(json.dumps(profile, indent=2, ensure_ascii=False) + '\n')
    print('recorded', PROFILE)


def package(output):
    profile, inputs, sources = collect()
    require(sources == profile['sources'], 'source set changed since the recorded validation')
    for name, item in profile['inputs'].items():
        require(sha(inputs[name]) == item['sha256'], f'validated input changed: {name}')

    module = inputs['module']
    probe = inputs['probe']
    require(module[:4] == b'\x7fELF' and module[4:6] == b'\x02\x01' and
            int.from_bytes(module[18:20], 'little') == 183, 'module is not ELF64 AArch64')
    require(probe[:4] == b'\x7fELF' and int.from_bytes(probe[18:20], 'little') == 183,
            'probe is not ELF64 AArch64')
    require(b'6.1.166-dirty' in module, 'module was not built with the pinned 6.1 DDK')

    check = plain(inputs['device-check'])
    require('[✓] PID namespace' in check and '[✓] IPC namespace' in check and
            'All required features found!' in check, 'device requirement check failed')
    probe_log = plain(inputs['device-probe'])
    for marker in PROBE_PASS:
        require(marker in probe_log, f'device probe missing: {marker}')
    require('FAIL' not in probe_log, 'device probe reported a failure')
    loader_log = plain(inputs['device-loader'])
    require('[不支持]' not in loader_log and '内核 BTF 与已验证的构建不一致' not in loader_log,
            'loader rejected the recorded device')
    device = profile['device']
    require(device['kernel_release'].encode() in inputs['common.sh'], 'loader release differs')
    require(device['btf_sha256'].encode() in inputs['common.sh'], 'loader BTF differs')
    require(sha(inputs['device_btf']) == device['btf_sha256'], 'recorded device BTF differs')

    files = {name: (ROOT / 'kernelsu-zorn' / name).read_bytes() for name in MODULE_FILES}
    files.update({'kernel/droidspaces_ns.ko': module, 'bin/nsprobe': probe})
    files['SHA256SUMS'] = ''.join(
        f'{sha(files[name])}  {name}\n' for name in sorted(files)
        if name.startswith(('kernel/', 'bin/'))).encode()
    files['LICENSE'] = (ROOT / 'LICENSE').read_bytes()
    files['README.md'] = (ROOT / 'docs/zorn-6.1.md').read_bytes()
    files['validation.json'] = (json.dumps(dict(
        release=profile['release'], device=device, build=profile['build'],
        inputs={name: item['sha256'] for name, item in profile['inputs'].items()},
        evidence={name: sha(inputs[name]) for name in EVIDENCE}), indent=2,
        ensure_ascii=False) + '\n').encode()
    files['SOURCE.txt'] = (
        'Built from src/port61full against the pinned android14-6.1 DDK.\n'
        f"module sha256: {sha(module)}\n"
        f"probe sha256: {sha(probe)}\n").encode()

    output.mkdir(parents=True, exist_ok=True)
    archive = output / f'Droidspaces-LKM-KernelSU-{profile["release"]}.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = (0o755 if name.endswith('.sh') or name.startswith('bin/')
                                  else 0o644) << 16
            z.writestr(info, data)
    standalone = {f'droidspaces_ns-{device["kmi"]}-zorn.ko': module}
    for name, data in standalone.items():
        (output / name).write_bytes(data)
    artifacts = sorted([archive, *(output / name for name in standalone)])
    (output / 'SHA256SUMS').write_text(
        ''.join(f'{sha(p.read_bytes())}  {p.name}\n' for p in artifacts))
    print('\n'.join(str(p) for p in [*artifacts, output / 'SHA256SUMS']))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record', action='store_true', help='refresh pinned hashes')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/release-zorn')
    args = parser.parse_args()
    record() if args.record else package(args.output)

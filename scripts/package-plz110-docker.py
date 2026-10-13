#!/usr/bin/env python3
"""Package the pinned PLZ110 v0.1.5 payloads after checking recorded evidence."""
import argparse
import hashlib
import json
import re
import runpy
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'build/plz110-0.1.5.json'
ARM64 = runpy.run_path(str(ROOT / 'scripts/package-module.py'))['arm64']
PAYLOADS = {
    'namespace': ('droidspaces_ns.ko', 'droidspaces_ns-6.12.69-canoe.ko'),
    'devtmpfs': ('droidspaces_devtmpfs.ko', 'droidspaces_devtmpfs-6.12.69-plz110.ko'),
    'compat': ('droidspaces_compat.ko', 'droidspaces_compat-6.12.69-plz110.ko'),
    'netfilter': ('droidspaces_netfilter.ko', 'droidspaces_netfilter-6.12.69-plz110.ko'),
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def package(output):
    profile = json.loads(PROFILE.read_text())
    require(profile['version'] == 'v0.1.5', 'unexpected profile')
    for name, expected in profile['sources'].items():
        require(sha((ROOT / name).read_bytes()) == expected, f'source changed: {name}')
    inputs = {}
    for name, item in profile['inputs'].items():
        data = (ROOT / item['path']).read_bytes()
        require(sha(data) == item['sha256'], f'validated input changed: {name}')
        inputs[name] = data
    for name in PAYLOADS:
        ARM64(inputs[name])
    ARM64(inputs['health'], executable=True)
    for name, check in profile['evidence_checks'].items():
        log = inputs[name].decode()
        require(all(marker in log for marker in check['markers']), f'incomplete evidence: {name}')
        if check.get('reject_kernel_warnings'):
            require(not re.search(r'WARNING: CPU:|BUG:|Oops:|Kernel panic|refcount_t:', log),
                    f'kernel regression: {name}')
    for name, count in (('namespace_abi', 824), ('compat_abi', 228), ('netfilter_abi', 734)):
        abi = json.loads(inputs[name])
        require(abi['matching_aggregates'] == count and not abi['imports']['missing_symbols']
                and not abi['imports']['different_function_prototypes'], f'ABI mismatch: {name}')
    device = profile['validation']['device']
    require(device['result'] == 'passed' and device['first_unlock'] == 'passed'
            and device['pid_live_after_cleanup'] == 0 and device['userns_live_after_cleanup'] == 0
            and device['devtmpfs_errors'] == 0 and device['selinux'] == 'Enforcing',
            'device validation incomplete')
    files = {name: (ROOT / 'kernelsu-plz110' / name).read_bytes() for name in
             ('module.prop', 'common.sh', 'customize.sh', 'post-fs-data.sh', 'service.sh', 'skip_mount')}
    require({'version=v0.1.5', 'versionCode=7', 'author=Shikochin', 'id=droidspaces_lkm'} <=
            set(files['module.prop'].decode().splitlines()), 'unexpected package identity')
    require(profile['release'].encode() in files['common.sh'] and
            sha(inputs['device_btf']).encode() in files['common.sh'], 'loader target differs')
    require(b'touch "$MODDIR/disable"' not in files['service.sh'], 'test boot guard in release')
    files.update({f'kernel/{names[0]}': inputs[key] for key, names in PAYLOADS.items()})
    files['bin/userns-check'] = inputs['health']
    files['SHA256SUMS'] = ''.join(f'{sha(files[name])}  {name}\n' for name in
                                 sorted(files) if name.startswith(('kernel/', 'bin/'))).encode()
    files['LICENSE'] = (ROOT / 'LICENSE').read_bytes()
    files['README.md'] = (ROOT / 'docs/releases-v0.1.5.md').read_bytes()
    files['validation.json'] = (json.dumps(profile, indent=2, ensure_ascii=False) + '\n').encode()
    files['SOURCE.txt'] = (
        'Source: https://github.com/Shikochin/Droidspaces-LKM/tree/v0.1.5\n'
        'Namespace and FUSE: src/port/ and src/userns/.\n'
        'devtmpfs: unchanged v0.1.1 payload; src/devtmpfs/ORIGIN.\n'
        'PID ownership repair: src/compat/ORIGIN; vendor ELF is not distributed.\n'
        'Netfilter: src/netfilter/ORIGIN and upstream/SOURCES.json.\n'
        'Binary, source, kernel and evidence hashes: validation.json.\n'
        'Device logs, HMA and vendor test fixtures are not distributed.\n'
    ).encode()
    output.mkdir(parents=True, exist_ok=True)
    archive = output / 'Droidspaces-LKM-KernelSU-v0.1.5-PLZ110.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 10, 0, 0, 0))
            info.create_system = 3
            executable = name.endswith('.sh') or name == 'bin/userns-check'
            info.external_attr = (0o100755 if executable else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    artifacts = [archive]
    for key, names in PAYLOADS.items():
        path = output / names[1]
        path.write_bytes(inputs[key])
        artifacts.append(path)
    (output / 'SHA256SUMS').write_text(''.join(f'{sha(p.read_bytes())}  {p.name}\n'
                                             for p in sorted(artifacts)))
    print('\n'.join(str(p) for p in [*artifacts, output / 'SHA256SUMS']))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'out/release-v0.1.5')
    package(parser.parse_args().output)

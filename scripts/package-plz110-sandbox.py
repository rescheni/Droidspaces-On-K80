#!/usr/bin/env python3
"""Package the exact PLZ110 sandbox binaries and evidence validated for v0.1.4."""
import argparse
import hashlib
import json
import re
import runpy
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'build/plz110-0.1.4.json'
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
    ARM64(inputs['health'], executable=True)
    markers = ('DS_USERNS_LIVE_PASS:', 'DS_USERNS_POLICY_PASS:',
               'DS_USERNS_MAPPING_RACE_PASS: 32', 'DS_USERNS_SANDBOX_PASS:',
               'DS_USERNS_SANDBOX_ALL_PASS:', 'DS_USERNS_SANDBOX_DISABLED_PASS:',
               'DS_USERNS_DROIDSPACES_PASS:', 'DS_FUSE_PASS', 'DS_FUSE_SANDBOX_MOUNT_PASS',
               'DS_FUSE_IO_PASS: unprivileged', 'DS_FUSE_IO_PASS: mapped-range',
               'DS_FUSE_ACL_PASS:', 'DS_FUSE_RETAINED_PASS:', 'DS_FUSE_BUBBLEWRAP_PASS')
    for name in ('gki_console', 'plz110_console', 'device_console'):
        log = inputs[name].decode()
        require(all(marker in log for marker in markers), f'incomplete validation: {name}')
        require(log.count('DS_FUSE_BUBBLEWRAP_PASS') == 2, f'FUSE sandbox/container validation missing: {name}')
        require(not re.search(r'BUG: |WARNING: CPU:|Oops:|Kernel panic|FAIL line|FAIL userns', log),
                f'failed validation: {name}')
        if name == 'device_console':
            require('DS_USERNS_DEVICE_PASS' in log and 'Enforcing' in log, 'hardware not validated')
        else:
            require('DS_USERNS_LIVE_VM_RESULT=0' in log and
                    'preserving HMA setuid entry; replacing native resume body' in log,
                    f'HMA VM validation missing: {name}')
    require('DS_SANDBOX_READY' in inputs['health_console'].decode(), 'health probe failed')
    require(inputs['loader_console'].decode().count('DS_SANDBOX_READY') == 2 and
            'After deferred namespace cleanup: 0' in inputs['loader_console'].decode(),
            'loader idempotence/cleanup failed')
    require('DS_DEVTMPFS_PASS:' in inputs['devtmpfs_console'].decode(), 'devtmpfs regression failed')
    require(all(x in inputs['pid_ipc_console'].decode() for x in
                ('DS_NAMESPACES_PASS:', 'DS_SYSVIPC_PASS:', 'DS_MQUEUE_PASS:')),
            'PID/IPC regression failed')
    abi = json.loads(inputs['abi'])
    require(abi['matching_aggregates'] == 824 and not abi['imports']['missing_symbols'] and
            not abi['imports']['different_function_prototypes'], 'ABI comparison failed')
    boot = inputs['boot_unlock_console'].decode()
    require('true' in boot.splitlines() and '/dev/fuse /mnt/user/0/emulated fuse ' in boot and
            '/dev/fuse /storage/emulated fuse ' in boot and 'Enforcing' in boot,
            'real first-unlock FUSE validation missing')
    require('DS_SANDBOX_READY' in inputs['boot_load_console'].decode(), 'boot loader check missing')
    validation = profile['validation']
    require(validation['device']['result'] == 'passed' and
            validation['device']['boot_and_first_unlock'] == 'passed' and
            validation['device']['userns_live_after_cleanup'] == 0 and
            validation['device']['devtmpfs_errors'] == 0 and
            validation['device']['selinux'] == 'Enforcing', 'hardware evidence incomplete')
    files = {name: (ROOT / 'kernelsu-plz110' / name).read_bytes() for name in
             ('module.prop', 'common.sh', 'customize.sh', 'post-fs-data.sh', 'service.sh', 'skip_mount')}
    require({'version=v0.1.4', 'versionCode=6', 'author=Shikochin', 'id=droidspaces_lkm'} <=
            set(files['module.prop'].decode().splitlines()), 'unexpected package identity')
    require(profile['release'].encode() in files['common.sh'] and
            sha(inputs['device_btf']).encode() in files['common.sh'], 'loader target differs')
    files.update({'kernel/droidspaces_ns.ko': inputs['namespace'],
                  'kernel/droidspaces_devtmpfs.ko': inputs['devtmpfs'],
                  'bin/userns-check': inputs['health']})
    files['SHA256SUMS'] = ''.join(f'{sha(files[name])}  {name}\n' for name in
                                 sorted(files) if name.startswith(('kernel/', 'bin/'))).encode()
    files['LICENSE'] = (ROOT / 'LICENSE').read_bytes()
    files['README.md'] = (ROOT / 'docs/releases-v0.1.4.md').read_bytes()
    files['validation.json'] = (json.dumps(profile, indent=2, ensure_ascii=False) + '\n').encode()
    files['SOURCE.txt'] = (
        'Source: https://github.com/Shikochin/Droidspaces-LKM/tree/v0.1.4\n'
        'PID/IPC/user namespace: src/port/ and src/userns/; see their ORIGIN files.\n'
        'devtmpfs: unchanged v0.1.1 payload; see src/devtmpfs/ORIGIN.\n'
        'Boot health probe: src/health/userns.c; GPL-2.0-only.\n'
        'Source, binary, kernel and evidence SHA256: validation.json.\n'
        'HMA is tested as an external module; no HMA payload is distributed.\n'
    ).encode()
    output.mkdir(parents=True, exist_ok=True)
    archive = output / 'Droidspaces-LKM-KernelSU-v0.1.4-PLZ110.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 10, 0, 0, 0))
            info.create_system = 3
            executable = name.endswith('.sh') or name == 'bin/userns-check'
            info.external_attr = (0o100755 if executable else 0o100644) << 16
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
    parser.add_argument('--output', type=Path, default=ROOT / 'out/release-v0.1.4')
    package(parser.parse_args().output)

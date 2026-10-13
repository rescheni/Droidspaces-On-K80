#!/usr/bin/env python3
"""Package a manually loaded PLC110 test module, never a boot-time installer."""
import argparse
import hashlib
from pathlib import Path
import runpy
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]
NAME = 'droidspaces_ns-6.6.118-plc110-test.ko'
VERSION = b'vermagic=6.6.118-android15-8-ge58033dc8ea6-abogki498046332-4k SMP preempt mod_unload modversions aarch64'


def package(module, destination):
    data = Path(module).read_bytes()
    modinfo = runpy.run_path(str(ROOT / 'scripts/abi/prepare-device-vm.py'))['modinfo']
    entries = modinfo(data)[2].split(b'\0')
    if [s for s in entries if s.startswith(b'vermagic=')] != [VERSION]:
        raise ValueError('module vermagic must match the recorded PLC110 kernel')
    if b'parmtype=plc110_test:bool' not in entries:
        raise ValueError('module does not support the explicit device-test parameter')
    dest = Path(destination)
    bundle = dest / 'PLC110-6.6.118-test'
    bundle.mkdir(parents=True, exist_ok=True)
    (bundle / NAME).write_bytes(data)
    digest = hashlib.sha256(data).hexdigest()
    (bundle / 'SHA256SUMS').write_text(f'{digest}  {NAME}\n')
    shutil.copy2(ROOT / 'scripts/load-plc110-test.sh', bundle / 'load-plc110-test.sh')
    shutil.copy2(ROOT / 'docs/plc110-device-test.md', bundle / 'README.md')
    archive = dest / 'Droidspaces-LKM-PLC110-6.6.118-test.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as out:
        for name in [NAME, 'SHA256SUMS', 'load-plc110-test.sh', 'README.md']:
            out.write(bundle / name, f'{bundle.name}/{name}')
    print(f'Module: {bundle / NAME}\nSHA256: {digest}\nArchive: {archive}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    package(args.module, args.output)

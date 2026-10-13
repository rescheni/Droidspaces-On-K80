#!/usr/bin/env python3
"""Check the real release ZIP and route all six payloads using the ARM64 ABI helper.

Runs in the VM tooling container. Only ksud and kernel identity are simulated;
no module is inserted into the host kernel.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def check(archive):
    with tempfile.TemporaryDirectory(prefix='release-check-', dir=ROOT/'out') as tmp:
        bundle = Path(tmp)
        with zipfile.ZipFile(archive) as z:
            if z.testzip() is not None:
                raise ValueError('ZIP integrity check failed')
            for entry in z.infolist():
                path = Path(entry.filename)
                if path.is_absolute() or '..' in path.parts:
                    raise ValueError('invalid ZIP member')
                z.extract(entry, bundle)
                (bundle/path).chmod((entry.external_attr >> 16) & 0o777)
        targets = json.loads((ROOT/'build/gki-targets.json').read_text())
        rows = (bundle/'kernel/manifest.tsv').read_text().splitlines()
        if len(rows) != 6 or {r.split('|')[0] for r in rows} != {t['kmi'] for t in targets.values()}:
            raise ValueError('release does not contain the six KMI targets')
        evidence = json.loads((bundle/'validation.json').read_text())
        for family, target in targets.items():
            name = f'droidspaces_ns-{target["kmi"]}-gki.ko'
            data = (bundle/'kernel'/name).read_bytes()
            if data != (archive.parent/name).read_bytes():
                raise ValueError('standalone and bundled modules differ')
            if hashlib.sha256(data).hexdigest() != evidence[family]['validation']['module_sha256']:
                raise ValueError('bundled module differs from tested module')
        script = r'''
set -eu
cp -a /bundle /tmp/module
export MODDIR=/tmp/module
cat > /tmp/ksud <<'KSUD'
#!/bin/sh
if [ "$1" = module ]; then exit 1; fi
[ "$1" = insmod ] && [ "$2" = "$EXPECTED_PAYLOAD" ] && [ "$3" = gki=1 ] && [ "$4" = activate=1 ] || exit 20
mkdir -p /tmp/module-sys/parameters /tmp/module-ns
printf 'Y\n' > /tmp/module-sys/parameters/activate
touch /tmp/module-ns/pid /tmp/module-ns/pid_for_children /tmp/module-ns/ipc
KSUD
chmod +x /tmp/ksud
. "$MODDIR/common.sh"
DS_KSUD=/tmp/ksud
DS_MODULE_SYS=/tmp/module-sys
DS_PROC_NS=/tmp/module-ns
uname() { if [ "$1" = -m ]; then echo aarch64; else echo "$TEST_RELEASE"; fi; }
while IFS='|' read -r kmi payload abi notes; do
    rm -rf /tmp/module-sys /tmp/module-ns
    TEST_RELEASE="${kmi#*-}.99-${kmi%%-*}-0-g123"
    DS_BTF="/work/out/gki/$kmi/kernel.btf"
    DS_KERNEL_NOTES="/work/out/gki/$kmi/kernel.notes"
    if [ "$kmi" = android12-5.10 ]; then DS_BTF=/missing-runtime-btf; fi
    export EXPECTED_PAYLOAD="$MODDIR/kernel/$payload"
    ds_load > /tmp/loader.log 2>&1 || { cat /tmp/loader.log; exit 1; }
    grep -F '[已加载]' "$MODDIR/status.txt"
    grep -F "$kmi" "$MODDIR/status.txt" >/dev/null
    test -f /tmp/module-sys/parameters/activate
done < "$MODDIR/kernel/manifest.tsv"
echo 'RELEASE_ROUTING_PASS: six real payloads, checksums, ABI helper, status'
'''
        subprocess.run(['docker','run','--rm','--platform','linux/arm64',
                        '-v',f'{ROOT}:/work:ro','-v',f'{bundle}:/bundle:ro',
                        'droidspaces-lkm-vm:local','sh','-c',script],check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive',type=Path)
    check(parser.parse_args().archive.resolve())

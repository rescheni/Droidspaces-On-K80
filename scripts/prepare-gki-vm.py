#!/usr/bin/env python3
"""Extract a pinned GKI test Image, config and BTF and audit the compiled module."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGETS = json.loads((ROOT / 'build/gki-targets.json').read_text())

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=TARGETS)
    args = parser.parse_args()
    target = TARGETS[args.target]
    out = ROOT / 'out/gki' / target['kmi']
    if not (out / 'droidspaces_ns.ko').is_file():
        parser.error('build the selected target first')
    script = '''set -eu
out=/module/out/gki/"$1"
llvm-objcopy -O binary -R .note -R .note.gnu.build-id -R .comment -S "$KDIR/vmlinux" "$out/Image"
if llvm-readelf -S "$KDIR/vmlinux" | grep -q ' \.BTF '; then
    llvm-objcopy --dump-section .BTF="$out/kernel.btf" "$KDIR/vmlinux" /tmp/vmlinux-copy
else
    # Older GKI configurations omit runtime BTF; recover reference types from DWARF.
    pahole -F dwarf --btf_gen_optimized --btf_encode_detached="$out/kernel.btf" "$KDIR/vmlinux"
fi
llvm-objcopy --dump-section .notes="$out/kernel.notes" "$KDIR/vmlinux" /tmp/vmlinux-notes
cp "$KDIR/.config" "$out/kernel.config"
cp "$KDIR/include/config/kernel.release" "$out/kernel.release"
pahole -F dwarf --btf_gen_optimized --btf_encode_detached="$out/module.btf" "$out/droidspaces_ns.ko"
llvm-nm -u "$out/droidspaces_ns.ko" > "$out/imports.txt"
llvm-nm "$KDIR/vmlinux" > "$out/symbols.txt"
'''
    subprocess.run(['docker','run','--rm','--platform','linux/amd64','-v',f'{ROOT}:/module',
                    target['image'],'sh','-c',script,'prepare-gki-vm',target['kmi']],check=True)
    subprocess.run(['python3',str(ROOT/'scripts/abi/make-constraints.py'),
                    '--module',str(out/'module.btf'),'--kernel',str(out/'kernel.btf'),
                    '--imports',str(out/'imports.txt'),'--symbols',str(out/'symbols.txt'),
                    '--port',str(ROOT/target['source']),
                    '--output',str(out/(target['kmi']+'.abi'))],check=True)
    (out/'prepared.json').write_text(json.dumps({name: hashlib.sha256((out/name).read_bytes()).hexdigest()
        for name in ('droidspaces_ns.ko','Image','kernel.config','kernel.release',
                     'kernel.btf','module.btf','kernel.notes',target['kmi']+'.abi',target['kmi']+'.audit.json')},indent=2)+'\n')

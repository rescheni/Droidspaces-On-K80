#!/usr/bin/env python3
"""Run a GKI module's complete VM suite and bind the result to input hashes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TARGETS = json.loads((ROOT / 'build/gki-targets.json').read_text())


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=TARGETS)
    args = parser.parse_args()
    target = TARGETS[args.target]
    out = ROOT / 'out/gki' / target['kmi']
    config = (out/'kernel.config').read_text().splitlines()
    hooks = (ROOT / target['source'] / 'hooks.c').read_text()
    module, image = out/'droidspaces_ns.ko', out/'Image'
    before = dict(module_sha256=sha(module), image_sha256=sha(image))
    env = {**os.environ, 'DS_VM_KERNEL':f'/work/out/gki/{target["kmi"]}/Image',
           'DS_VM_MODULE':f'/work/out/gki/{target["kmi"]}/droidspaces_ns.ko',
           'DS_VM_LOG_DIR':f'out/gki/{target["kmi"]}/vm', 'DS_VM_MODULE_PARAMS':'gki=1',
           'DS_VM_GKI_DIR':f'/work/out/gki/{target["kmi"]}',
           'DS_TEST_618_INTERFACES':str(int(args.target == '6.18')),
           'DS_TEST_BPF_ITER':str(int('CONFIG_DEBUG_INFO_BTF=y' in config)),
           'DS_VM_NATIVE_IPC':str(int('CONFIG_IPC_NS=y' in config)),
           'DS_TEST_PIDFD_NS_IOCTL':str(int('pidfd_ioctl' in hooks)),
           'DS_TEST_MEMFD_POLICY':str(int('IPC_SYSCALL(memfd_create)' in hooks)),
           'DS_TEST_SHM_HUGETLB_REJECT':'1'}
    record = out/'validation.json'
    record.unlink(missing_ok=True)
    result = subprocess.run([str(ROOT/'tests/vm/run.sh')],env=env)
    if result.returncode:
        raise SystemExit(result.returncode)
    if before != dict(module_sha256=sha(module), image_sha256=sha(image)):
        raise RuntimeError('VM inputs changed while tests ran')
    record.write_text(json.dumps(dict(kmi=target['kmi'], **before,
        console_sha256=sha(out/'vm/console.log'), result='passed',
        options={k:v for k,v in env.items() if k.startswith('DS_')}),indent=2)+'\n')

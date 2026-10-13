#!/usr/bin/env python3
"""Build the static ARM64 pre-load ABI checker in the test runtime image."""
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'out/gki/bin'

if __name__ == '__main__':
    OUT.mkdir(parents=True, exist_ok=True)
    source = ROOT/'src/abi/check.c'
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    subprocess.run(['docker','run','--rm','--platform','linux/arm64','-v',f'{ROOT}:/work',
        'droidspaces-lkm-vm:local','gcc','-static','-O2','-Wall','-Wextra','-Werror',
        '/work/src/abi/check.c','-o','/work/out/gki/bin/abi-check'],check=True)
    if digest != hashlib.sha256(source.read_bytes()).hexdigest():
        raise RuntimeError('ABI checker source changed during build')
    (OUT/'build.json').write_text(json.dumps(dict(source_sha256=digest,
        binary_sha256=hashlib.sha256((OUT/'abi-check').read_bytes()).hexdigest()),indent=2)+'\n')

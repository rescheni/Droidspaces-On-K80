#!/usr/bin/env python3
"""Build one independent GKI namespace module using its pinned DDK image."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGETS = json.loads((ROOT / 'build/gki-targets.json').read_text())

def source_hashes(target):
    paths = [ROOT/'Makefile', ROOT/'src/gki-target.h']
    paths += [p for directory in (ROOT/target['source'], ROOT/'vendor/kallrecon')
              for p in directory.rglob('*') if p.is_file()
              and not {'.git', '.DS_Store', '__pycache__'}.intersection(p.parts)]
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(paths)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=TARGETS)
    parser.add_argument('make_args', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    target = TARGETS[args.target]
    source = ROOT / target['source']
    if not (source / 'main.c').is_file():
        parser.error(f'missing independent source set: {source}')
    cmd = ['docker', 'run', '--rm', '--platform', 'linux/amd64',
           '-v', f'{ROOT}:/module', '-w', '/module', target['image'],
           'make', 'TARGET=namespace', f'PORT_DIR={target["source"]}',
           f'GKI_ANDROID={target["kmi"].split("-")[0]}', f'GKI_VERSION={args.target}',
           f'OUT=/module/out/gki/{target["kmi"]}', *args.make_args]
    out = ROOT/'out/gki'/target['kmi']
    record = out/'build.json'
    record.unlink(missing_ok=True)
    before = source_hashes(target)
    subprocess.run(cmd, check=True)
    if before != source_hashes(target):
        raise RuntimeError('build sources changed during compilation')
    record.write_text(json.dumps(dict(target=target, sources=before,
        module_sha256=hashlib.sha256((out/'droidspaces_ns.ko').read_bytes()).hexdigest()),
        indent=2)+'\n')

#!/usr/bin/env python3
"""Emit the layouts and function prototypes a module consumes from its GKI host."""
import argparse
import importlib.util
import hashlib
import json
import re
import struct
from pathlib import Path

spec = importlib.util.spec_from_file_location('btf', Path(__file__).with_name('compare-btf.py'))
btf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(btf)


def prototype_hash(types, ident):
    value = 14695981039346656037
    def add(data):
        nonlocal value
        for byte in data:
            value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    def word(n):
        add(struct.pack('<I', n))
    def visit(i, depth=0):
        if depth > 128:
            raise ValueError('recursive prototype')
        t = types[i]
        kind = t['kind']
        if kind in (8, 9, 10, 11, 18):
            visit(t['size'], depth + 1)
        elif not i:
            word(0)
        elif kind == 2:
            word(2); visit(t['size'], depth + 1)
        elif kind == 13:
            word(13); visit(t['size'], depth + 1); word(len(t['data']) // 2)
            for j in range(1, len(t['data']), 2):
                visit(t['data'][j], depth + 1)
        elif kind == 1:
            word(1); word(t['size']); word(t['data'][0])
        elif kind in (4, 5, 7):
            word(4); add(t['name'].encode() + b'\0')
        elif kind == 3:
            word(3); visit(t['data'][0], depth + 1); word(t['data'][2])
        else:
            word(kind); add(t['name'].encode() + b'\0'); word(t['size'])
    visit(ident)
    return value


def audit_hooks(module, kernel, hooks):
    native, compiled = kernel.functions(), module.functions()
    aliases = {'ds_proc_ns_dir_operations.iterate_shared': 'proc_ns_dir_readdir',
               'ds_proc_ns_dir_inode_operations.lookup': 'proc_ns_dir_lookup'}
    pairs = [(name, aliases.get(target, target)) for name, target in
             re.findall(r'HOOK(?:_ORIGINAL)?\("([^"]+)",\s*([\w.]+)', hooks)]
    pairs += [('init_seq_pidns', 'ds_bpf_init_seq_pidns'),
              ('fini_seq_pidns', 'ds_bpf_fini_seq_pidns')]
    # All arm64 syscall wrappers have the same ABI, including ni_syscall aliases.
    syscalls = set(re.findall(r'IPC_SYSCALL(?:_DIRECT)?\((\w+)\)', hooks)) - {'name'}
    for name in sorted(syscalls):
        target = f'ds_guard_{name}' if f'ds_guard_{name}' in compiled else f'__arm64_sys_ds_{name}'
        pairs.append(('__arm64_sys_setns', target))
    mismatches = {}
    for name, target in pairs:
        if name not in native or target not in compiled:
            mismatches[target] = dict(native=name, error='missing function prototype')
        elif native[name] != compiled[target]:
            mismatches[target] = dict(native=name, expected=native[name], actual=compiled[target])
    if mismatches:
        raise ValueError(f'incompatible hook prototypes: {mismatches}')
    return len(pairs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ('module', 'kernel', 'imports', 'symbols', 'port', 'output'):
        parser.add_argument('--' + field, type=Path, required=True)
    args = parser.parse_args()
    module, kernel = btf.BTF(args.module), btf.BTF(args.kernel)
    report = btf.compare(args.module, args.kernel)
    if report['differences']:
        raise ValueError(f'module has incompatible host layouts: {report["differences"]}')
    hooks = (args.port / 'hooks.c').read_text()
    checked_hooks = audit_hooks(module, kernel, hooks)
    lines = ['DSABI1']
    native = kernel.aggregates()
    for name, (size, fields) in sorted(module.aggregates().items()):
        if name not in native:
            continue
        lines.append(f'S {name} {size} {len(fields)}')
        for member, (offset, bits) in sorted(fields.items()):
            lines.append(f'F {member or "-"} {offset} {bits}')
    wanted = {line.split()[-1] for line in args.imports.read_text().splitlines() if line.strip()}
    imports = set(wanted)
    available = {re.split(r'\$|\.llvm\.', line.split()[-1])[0]
                 for line in args.symbols.read_text().splitlines() if line.strip()}
    if imports - available:
        raise ValueError(f'unresolved kernel imports: {sorted(imports - available)}')
    wanted.update(re.findall(r'HOOK(?:_ORIGINAL)?\("([^"]+)"', hooks))
    wanted.update(('init_seq_pidns', 'fini_seq_pidns', '__arm64_sys_setns'))
    functions = {t['name']: t['size'] for t in kernel.types if t['kind'] == 12}
    for name in sorted(wanted & functions.keys()):
        lines.append(f'P {name} {prototype_hash(kernel.types, functions[name]):016x}')
    args.output.write_text('\n'.join(lines) + '\n')
    args.output.with_suffix('.audit.json').write_text(json.dumps(dict(
        checked_hook_prototypes=checked_hooks, resolved_imports=len(imports),
        matching_aggregates=report['matching_aggregates'],
        module_btf_sha256=hashlib.sha256(args.module.read_bytes()).hexdigest(),
        kernel_btf_sha256=hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
        constraints_sha256=hashlib.sha256(args.output.read_bytes()).hexdigest()), indent=2)+'\n')
    print(f'{sum(s.startswith("S ") for s in lines)} layouts, '
          f'{sum(s.startswith("P ") for s in lines)} prototypes -> {args.output}')

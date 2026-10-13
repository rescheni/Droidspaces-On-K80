#!/usr/bin/env python3
"""Compare aggregate layouts against a device's raw BTF. This is not an ABI proof."""
import argparse
import json
import struct
from pathlib import Path

class BTF:
    def __init__(self, path):
        data = Path(path).read_bytes()
        magic, version, flags, hdr, toff, tlen, soff, slen = struct.unpack_from('<HBBIIIII', data)
        if magic != 0xeb9f or version != 1:
            raise ValueError('expected little-endian raw BTF version 1')
        strings = data[hdr + soff:hdr + soff + slen]
        def name(offset):
            return strings[offset:strings.index(0, offset)].decode()
        self.types = [dict(kind=0, name='void', size=0, data=[])]
        pos, end = hdr + toff, hdr + toff + tlen
        while pos < end:
            noff, info, value = struct.unpack_from('<III', data, pos)
            kind, count, flag = (info >> 24) & 31, info & 65535, info >> 31
            pos += 12
            words = {1:1, 2:0, 3:3, 4:count*3, 5:count*3, 6:count*2,
                     7:0, 8:0, 9:0, 10:0, 11:0, 12:0, 13:count*2,
                     14:1, 15:count*3, 16:0, 17:1, 18:0, 19:count*3}[kind]
            payload = struct.unpack_from('<' + 'I'*words, data, pos)
            pos += words*4
            item = dict(kind=kind, name=name(noff), size=value, data=payload, flag=flag)
            if kind in (4, 5):
                item['members'] = [(name(payload[i]), payload[i+1], payload[i+2]) for i in range(0, words, 3)]
            self.types.append(item)
        if pos != end:
            raise ValueError('malformed BTF type section')

    def size(self, ident):
        t = self.types[ident]
        if t['kind'] in (8, 9, 10, 11, 18):
            return self.size(t['size'])
        if t['kind'] == 2:
            return 8
        if t['kind'] == 3:
            element, _, count = t['data']
            return count * self.size(element)
        return t['size'] if t['kind'] in (1, 4, 5, 6, 16, 19) else 0

    def fields(self, ident, base=0, prefix=''):
        t = self.types[ident]
        result = {}
        for name, typ, offset in t['members']:
            bits = offset >> 24 if t['flag'] else 0
            offset = offset & 0xffffff if t['flag'] else offset
            inner = self.types[typ]
            if not name and inner['kind'] in (4, 5):
                result.update(self.fields(typ, base + offset, prefix))
            else:
                result[prefix + name] = [base + offset, bits or self.size(typ)*8]
        return result

    def signature(self, ident):
        t = self.types[ident]
        kind = t['kind']
        if kind in (8, 9, 10, 11, 18):
            return self.signature(t['size'])
        if kind == 2:
            return ['ptr', self.signature(t['size'])]
        if kind == 13:
            return ['func', self.signature(t['size']),
                    [self.signature(t['data'][i+1]) for i in range(0, len(t['data']), 2)]]
        if kind == 1:
            return ['int', t['size'], t['data'][0]]
        if kind in (4, 5, 7):
            return ['aggregate', t['name']]
        if kind == 3:
            return ['array', self.signature(t['data'][0]), t['data'][2]]
        return [kind, t['name'], t['size']]

    def functions(self):
        return {t['name']: self.signature(t['size']) for t in self.types if t['kind'] == 12}

    def aggregates(self):
        return {t['name']: (t['size'], self.fields(i)) for i, t in enumerate(self.types)
                if t['kind'] in (4, 5) and t['name'] and t['size']}

def compare(module, device):
    left, right = BTF(module).aggregates(), BTF(device).aggregates()
    differences, same = {}, 0
    for name, (size, fields) in left.items():
        if name not in right:
            continue
        other_size, other_fields = right[name]
        changed = {n: [v, other_fields.get(n)] for n, v in fields.items() if other_fields.get(n) != v}
        added = {n: v for n, v in other_fields.items() if n not in fields}
        if size != other_size or changed or added:
            differences[name] = dict(size=[size, other_size], changed=changed, device_only=added)
        else:
            same += 1
    return dict(matching_aggregates=same, differences=differences,
                module_only=sorted(set(left)-set(right)))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module')
    parser.add_argument('device')
    parser.add_argument('--kernel-btf', help='DDK vmlinux BTF for imported function prototypes')
    parser.add_argument('--imports', help='llvm-nm -u output for this module')
    parser.add_argument('--symbols', help='device /proc/kallsyms (addresses may be zero)')
    args = parser.parse_args()
    result = compare(args.module, args.device)
    if any((args.kernel_btf, args.imports, args.symbols)):
        if not all((args.kernel_btf, args.imports, args.symbols)):
            parser.error('--kernel-btf, --imports and --symbols must be supplied together')
        imports = {line.split()[-1] for line in Path(args.imports).read_text().splitlines() if line.strip()}
        symbols = {line.split()[2] for line in Path(args.symbols).read_text().splitlines() if len(line.split()) >= 3}
        left = BTF(args.kernel_btf).functions()
        right = BTF(args.device).functions()
        common = imports & left.keys() & right.keys()
        result['imports'] = dict(count=len(imports), missing_symbols=sorted(imports-symbols),
            compared_function_prototypes=len(common),
            different_function_prototypes={n: [left[n], right[n]] for n in sorted(common) if left[n] != right[n]},
            without_comparable_function_prototype=sorted(imports-common))
    print(json.dumps(result, indent=2))

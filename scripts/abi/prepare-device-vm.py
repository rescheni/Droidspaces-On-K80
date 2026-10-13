#!/usr/bin/env python3
"""Prepare offline VM inputs from the user's Android boot image and reference module.

Only .modinfo is relocated, following ksuinit's vermagic replacement approach.
This does not establish ABI compatibility or make the module device-loadable.
"""
import argparse
import hashlib
import struct
from pathlib import Path


def modinfo(data):
    if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('expected ELF64 little-endian AArch64')
    offset = struct.unpack_from('<Q', data, 40)[0]
    entry, count, strings = struct.unpack_from('<HHH', data, 58)
    if entry != 64 or not 0 < strings < count or offset + entry * count > len(data):
        raise ValueError('invalid ELF section table')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i*entry) for i in range(count)]
    string_section = sections[strings]
    text = data[string_section[4]:string_section[4]+string_section[5]]
    for index, section in enumerate(sections):
        start = section[0]
        if text[start:text.index(0, start)] == b'.modinfo':
            start, size, alignment = section[4], section[5], max(section[8], 1)
            if start + size > len(data) or alignment & (alignment-1):
                raise ValueError('invalid modinfo bounds/alignment')
            return offset + index*entry, alignment, data[start:start+size]
    raise ValueError('no modinfo section')


def prepare(boot, module, reference, destination):
    data = Path(boot).read_bytes()
    if data[:8] != b'ANDROID!' or struct.unpack_from('<I', data, 40)[0] not in (3, 4):
        raise ValueError('expected Android boot image v3/v4')
    size = struct.unpack_from('<I', data, 8)[0]
    kernel = data[4096:4096+size]
    if len(kernel) != size or kernel[56:60] != b'ARM\x64':
        raise ValueError('expected an uncompressed ARM64 Image')
    _, _, reference_info = modinfo(Path(reference).read_bytes())
    versions = [s for s in reference_info.split(b'\0') if s.startswith(b'vermagic=')]
    expected = b'vermagic=6.12.69-android16-6-g242e1a07f878-ab15865446-4k '
    if len(versions) != 1 or not versions[0].startswith(expected):
        raise ValueError('reference module is not for the recorded target kernel')
    output = bytearray(Path(module).read_bytes())
    header, alignment, info = modinfo(output)
    entries = [s for s in info.split(b'\0') if s and not s.startswith(b'vermagic=')]
    entries.append(versions[0])
    info = b'\0'.join(entries) + b'\0'
    offset = (len(output) + alignment-1) & ~(alignment-1)
    output.extend(bytes(offset-len(output)))
    output.extend(info)
    struct.pack_into('<QQ', output, header + 24, offset, len(info))
    dest = Path(destination)
    dest.mkdir(parents=True, exist_ok=True)
    (dest/'Image').write_bytes(kernel)
    (dest/'droidspaces_ns.ko').write_bytes(output)
    print('Image SHA256:', hashlib.sha256(kernel).hexdigest())
    print('VM module SHA256:', hashlib.sha256(output).hexdigest())
    print(versions[0].decode())

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('boot', 'module', 'reference', 'destination'):
        parser.add_argument(name)
    args = parser.parse_args()
    prepare(args.boot, args.module, args.reference, args.destination)

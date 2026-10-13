#!/usr/bin/env python3
"""Prepare QEMU inputs from the recorded PLC110 6.6 boot image.

Requires the host lz4 CLI. Changes only the module's vermagic release prefix;
this does not establish ABI compatibility. Device opt-in is controlled by the
module source and its plc110_test parameter, not by vermagic.
"""
import argparse
import gzip
import hashlib
import json
import re
import runpy
import struct
import subprocess
import tempfile
from pathlib import Path

BOOT_SHA256 = 'c664a0ad0353da27ee65b16ade2b607912c98697b680489036489277746e2eec'
RELEASE = '6.6.118-android15-8-ge58033dc8ea6-abogki498046332-4k'
DDK_RELEASE = '6.6.127-4k-g46a034eca005-dirty'
FLAGS = ' SMP preempt mod_unload modversions aarch64'


def prepare(boot, module, destination):
    data = Path(boot).read_bytes()
    if hashlib.sha256(data).hexdigest() != BOOT_SHA256:
        raise ValueError('boot image does not match the recorded PLC110 input')
    if data[:8] != b'ANDROID!' or struct.unpack_from('<I', data, 40)[0] != 4:
        raise ValueError('expected Android boot image v4')
    size = struct.unpack_from('<I', data, 8)[0]
    kernel = data[4096:4096 + size]
    if len(kernel) != size or kernel[:4] != b'\x02\x21\x4c\x18':
        raise ValueError('expected a legacy LZ4 kernel')
    with tempfile.TemporaryDirectory() as directory:
        packed = Path(directory) / 'Image.lz4'
        unpacked = Path(directory) / 'Image'
        packed.write_bytes(kernel)
        subprocess.run(['lz4', '-d', str(packed), str(unpacked)], check=True)
        kernel = unpacked.read_bytes()
    if kernel[56:60] != b'ARM\x64' or (b'Linux version ' + RELEASE.encode() + b' ') not in kernel:
        raise ValueError('unexpected uncompressed kernel or release')
    start = kernel.index(b'IKCFG_ST') + 8
    end = kernel.index(b'IKCFG_ED', start)
    config = gzip.decompress(kernel[start:end])
    required = [b'CONFIG_SMP=y', b'CONFIG_PREEMPT=y', b'CONFIG_MODULE_UNLOAD=y',
                b'CONFIG_MODVERSIONS=y', b'CONFIG_ARM64_4K_PAGES=y']
    if any(line not in config.splitlines() for line in required):
        raise ValueError('kernel config does not match expected vermagic flags')
    btfs = []
    for match in re.finditer(b'\x9f\xeb\x01\x00\x18\x00\x00\x00', kernel):
        pos = match.start()
        if pos + 24 > len(kernel):
            continue
        header = struct.unpack_from('<HBBIIIII', kernel, pos)
        total = header[3] + header[6] + header[7]
        if (pos + total <= len(kernel) and header[4] == 0 and
                header[5] == header[6] and kernel[pos + header[3] + header[6]] == 0):
            btfs.append(kernel[pos:pos + total])
    if len(btfs) != 1:
        raise ValueError('expected exactly one embedded BTF')

    # Reuse the ELF bounds/alignment validation from the 6.12 VM preparer.
    modinfo = runpy.run_path(str(Path(__file__).with_name('prepare-device-vm.py')))['modinfo']
    output = bytearray(Path(module).read_bytes())
    header, alignment, info = modinfo(output)
    versions = [entry for entry in info.split(b'\0') if entry.startswith(b'vermagic=')]
    if versions != [('vermagic=' + DDK_RELEASE + FLAGS).encode()]:
        raise ValueError('module must be built with the pinned android15-6.6 DDK')
    entries = [entry for entry in info.split(b'\0') if entry and not entry.startswith(b'vermagic=')]
    entries.append(('vermagic=' + RELEASE + FLAGS).encode())
    info = b'\0'.join(entries) + b'\0'
    offset = (len(output) + alignment - 1) & ~(alignment - 1)
    output.extend(bytes(offset - len(output)))
    output.extend(info)
    struct.pack_into('<QQ', output, header + 24, offset, len(info))

    dest = Path(destination)
    dest.mkdir(parents=True, exist_ok=True)
    for name, content in [('Image', kernel), ('kernel.config', config),
                          ('kernel.btf', btfs[0]), ('droidspaces_ns.ko', output)]:
        (dest / name).write_bytes(content)
    report = dict(boot_sha256=BOOT_SHA256, release=RELEASE,
                  image_sha256=hashlib.sha256(kernel).hexdigest(),
                  module_sha256=hashlib.sha256(output).hexdigest(),
                  scope='recorded kernel; PLC110 device testing requires plc110_test=1')
    (dest / 'inputs.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('boot', 'module', 'destination'):
        parser.add_argument(name)
    args = parser.parse_args()
    prepare(args.boot, args.module, args.destination)

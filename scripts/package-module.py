#!/usr/bin/env python3
"""Package all six independently built and VM-validated GKI payloads."""
import argparse
import hashlib
import json
import re
import runpy
import struct
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = runpy.run_path(str(ROOT/'scripts/build-gki.py'))
TARGETS = BUILD['TARGETS']
MODULE_FILES = ('module.prop', 'common.sh', 'customize.sh', 'post-fs-data.sh',
                'service.sh', 'skip_mount')
REQUIRED = {'6.18', '6.12', '6.6', '6.1', '5.15', '5.10'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def arm64(data, executable=False):
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H',data,18)[0] != 183:
        raise ValueError('expected ELF64 little-endian AArch64')
    kind = struct.unpack_from('<H',data,16)[0]
    if kind not in ((2,3) if executable else (1,)):
        raise ValueError('unexpected ELF type')
    if executable:
        offset = struct.unpack_from('<Q',data,32)[0]
        size, count = struct.unpack_from('<HH',data,54)
        if size != 56 or offset + size*count > len(data):
            raise ValueError('invalid program headers')
        for i in range(count):
            if struct.unpack_from('<I',data,offset+i*size)[0] == 3:
                raise ValueError('ABI checker must be static; found PT_INTERP')


def validated_payload(target):
    out = ROOT/'out/gki'/target['kmi']
    payload = (out/'droidspaces_ns.ko').read_bytes()
    arm64(payload)
    build = json.loads((out/'build.json').read_text())
    if build['target'] != target or build['sources'] != BUILD['source_hashes'](target):
        raise ValueError(f'{target["kmi"]}: sources changed since build')
    if build['module_sha256'] != sha(payload):
        raise ValueError('build module hash mismatch')
    prepared = json.loads((out/'prepared.json').read_text())
    names = ('droidspaces_ns.ko','Image','kernel.config','kernel.release',
             'kernel.btf','module.btf','kernel.notes',target['kmi']+'.abi',target['kmi']+'.audit.json')
    if prepared != {name:sha((out/name).read_bytes()) for name in names}:
        raise ValueError('ABI preparation inputs changed')
    if 'CONFIG_ARM64_4K_PAGES=y' not in (out/'kernel.config').read_text().splitlines():
        raise ValueError('unsupported kernel page size')
    validation = json.loads((out/'validation.json').read_text())
    log = (out/'vm/console.log').read_bytes()
    if (validation['result'] != 'passed' or validation['kmi'] != target['kmi'] or
            validation['module_sha256'] != sha(payload) or
            validation['image_sha256'] != prepared['Image'] or
            validation['console_sha256'] != sha(log)):
        raise ValueError('VM evidence does not match packaged payload')
    options = validation['options']
    if options['DS_VM_MODULE_PARAMS'] != 'gki=1' or options['DS_TEST_SHM_HUGETLB_REJECT'] != '1':
        raise ValueError('VM suite used unexpected loading or IPC options')
    markers = ['DS_VM_RESULT=0', 'DS_NAMESPACES_PASS:', 'DS_PID_LIFETIMES_PASS:',
               'DS_SYSVIPC_PASS:', 'DS_MQUEUE_PASS:', 'remaining pid=0 ipc=0 ipc_tasks=0',
               'DS_ABI_PREFLIGHT_PASS:']
    if target['kmi'] == 'android17-6.18':
        if options.get('DS_TEST_618_INTERFACES') != '1':
            raise ValueError('6.18 native interface tests were not run')
        markers.append('DS_618_INTERFACES_PASS:')
    native_btf = 'CONFIG_DEBUG_INFO_BTF=y' in (out/'kernel.config').read_text().splitlines()
    if options.get('DS_TEST_BPF_ITER','1') != str(int(native_btf)):
        raise ValueError('BPF iterator test configuration does not match the baseline')
    if options['DS_VM_NATIVE_IPC'] == '1': markers.append('DS_PREEXISTING_IPC_PASS:')
    if options['DS_TEST_MEMFD_POLICY'] == '1': markers.append('DS_PID_POLICY_PASS:')
    text = log.decode()
    if any(marker not in text for marker in markers) or re.search(r'BUG:|WARNING:|Oops|Kernel panic',text):
        raise ValueError('incomplete or failed VM suite')
    modinfo = runpy.run_path(str(ROOT/'scripts/abi/prepare-device-vm.py'))['modinfo'](payload)[2].split(b'\0')
    if b'parmtype=gki:bool' not in modinfo:
        raise ValueError('module lacks the unified gki loading interface')
    audit = json.loads((out/(target['kmi']+'.audit.json')).read_text())
    if (audit['checked_hook_prototypes'] < 20 or audit['resolved_imports'] < 100 or
            audit['constraints_sha256'] != prepared[target['kmi']+'.abi'] or
            audit['kernel_btf_sha256'] != prepared['kernel.btf'] or
            audit['module_btf_sha256'] != prepared['module.btf']):
        raise ValueError('ABI audit does not match packaged constraints')
    evidence = dict(target=target, release=(out/'kernel.release').read_text().strip(),
                    validation=validation, audit=audit, build=build)
    return payload, (out/(target['kmi']+'.abi')).read_bytes(), evidence


def package(output):
    if set(TARGETS) != REQUIRED:
        raise ValueError('release requires exactly the six requested kernel families')
    metadata = (ROOT/'kernelsu/module.prop').read_text()
    version = re.search(r'^version=(v[0-9]+\.[0-9]+\.[0-9]+)$',metadata,re.M).group(1)
    files = {name:(ROOT/'kernelsu'/name).read_bytes() for name in MODULE_FILES}
    helper = (ROOT/'out/gki/bin/abi-check').read_bytes()
    arm64(helper, executable=True)
    helper_build = json.loads((ROOT/'out/gki/bin/build.json').read_text())
    if helper_build != dict(source_sha256=sha((ROOT/'src/abi/check.c').read_bytes()),binary_sha256=sha(helper)):
        raise ValueError('ABI checker source or binary changed since compilation')
    files['bin/abi-check'] = helper
    standalone, evidence, rows = {}, {}, []
    for family, target in TARGETS.items():
        ko, abi, evidence[family] = validated_payload(target)
        name = f'droidspaces_ns-{target["kmi"]}-gki.ko'
        abiname = target['kmi']+'.abi'
        standalone[name] = files['kernel/'+name] = ko
        files['kernel/'+abiname] = abi
        files['validation/'+target['kmi']+'.console.log'] = (ROOT/'out/gki'/target['kmi']/'vm/console.log').read_bytes()
        notesname = target['kmi']+'.notes'
        files['kernel/'+notesname] = (ROOT/'out/gki'/target['kmi']/'kernel.notes').read_bytes()
        rows.append(f'{target["kmi"]}|{name}|{abiname}|{notesname}\n')
    files['kernel/manifest.tsv'] = ''.join(rows).encode()
    checksums = ''.join(f'{sha(data)}  {name.removeprefix("kernel/")}\n'
                        for name,data in sorted(files.items()) if name.startswith('kernel/'))
    checksums += f'{sha(helper)}  ../bin/abi-check\n'
    files['kernel/SHA256SUMS'] = checksums.encode()
    files['LICENSE'] = (ROOT/'LICENSE').read_bytes()
    files['README.md'] = (ROOT/'README.md').read_bytes()
    files['validation.json'] = (json.dumps(evidence,indent=2)+'\n').encode()
    files['SOURCE.txt'] = (
        'Source: https://github.com/Shikochin/Droidspaces-LKM\n'
        f'Release: {version}\n'
        'Six independent GKI source sets; pinned build inputs in build/gki-targets.json.\n'
        'Validation and source hashes: validation.json. Original provenance: src/port*/ORIGIN.\n'
        'VM validation does not establish compatibility with every vendor kernel sharing a KMI.\n'
    ).encode()
    output.mkdir(parents=True,exist_ok=True)
    archive = output/f'Droidspaces-LKM-KernelSU-{version}.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for name,data in sorted(files.items()):
            entry = zipfile.ZipInfo(name,date_time=(2026,10,10,0,0,0))
            entry.create_system = 3
            executable = name.endswith('.sh') or name == 'bin/abi-check'
            entry.external_attr = (0o100755 if executable else 0o100644) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(entry,data)
    for name,data in standalone.items(): (output/name).write_bytes(data)
    artifacts = sorted([archive,*(output/name for name in standalone)])
    (output/'SHA256SUMS').write_text(''.join(f'{sha(p.read_bytes())}  {p.name}\n' for p in artifacts))
    print('\n'.join(str(p) for p in [*artifacts,output/'SHA256SUMS']))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'out/release-v0.2.0')
    package(parser.parse_args().output)

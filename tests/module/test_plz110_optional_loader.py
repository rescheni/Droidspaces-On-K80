#!/usr/bin/env python3
"""Check fixed-target two-module loading and partial failure reporting."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RELEASE = '6.12.69-android16-6-g242e1a07f878-ab15865446-4k'


class LoaderTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.module = self.root / 'module'
        shutil.copytree(ROOT / 'kernelsu-plz110', self.module)
        (self.module / 'kernel').mkdir()
        sums = []
        for name in ('droidspaces_ns.ko', 'droidspaces_devtmpfs.ko'):
            data = ('fake ' + name).encode()
            (self.module / 'kernel' / name).write_bytes(data)
            sums.append(f'{hashlib.sha256(data).hexdigest()}  {name}\n')
        (self.module / 'bin').mkdir()
        probe = self.module / 'bin/userns-check'
        probe.write_text('#!/bin/sh\n[ "$FAKE_MODE" != health_fail ]\n')
        probe.chmod(0o755)
        sums = [line.replace('  ', '  kernel/') for line in sums]
        sums.append(f'{hashlib.sha256(probe.read_bytes()).hexdigest()}  bin/userns-check\n')
        (self.module / 'SHA256SUMS').write_text(''.join(sums))
        (self.root / 'compatible').write_bytes(b'qcom,canoe\0qcom,test\0')
        (self.root / 'btf').write_bytes(b'fixed BTF fixture')
        self.btf_hash = hashlib.sha256((self.root / 'btf').read_bytes()).hexdigest()
        ksud = self.root / 'ksud'
        ksud.write_text('''#!/bin/sh
if [ "$1" = module ]; then
    [ "$FAKE_CONFIG" != fail ] || exit 1
    printf '%s\\n' "$6" > "$FAKE_ROOT/description"
    exit 0
fi
[ "$1" = insmod ] || exit 2
printf '%s\\n' "$*" >> "$FAKE_ROOT/calls"
case "$2" in
    */droidspaces_ns.ko)
        [ "$FAKE_MODE" != ns_error ] || exit 17
        [ "$3" = activate=1 ] && [ "$4" = userns_device_test=1 ] && [ "$#" = 4 ] || exit 18
        mkdir -p "$FAKE_ROOT/sys/parameters" "$FAKE_ROOT/ns"
        echo Y > "$FAKE_ROOT/sys/parameters/activate"
        echo Y > "$FAKE_ROOT/sys/parameters/userns_device_test"
        echo 0 > "$FAKE_ROOT/sys/parameters/userns_live"
        touch "$FAKE_ROOT/ns/user"
        touch "$FAKE_ROOT/ns/pid" "$FAKE_ROOT/ns/pid_for_children" "$FAKE_ROOT/ns/ipc"
        ;;
    */droidspaces_devtmpfs.ko)
        [ "$FAKE_MODE" != dev_error ] || exit 19
        [ "$#" = 2 ] || exit 20
        mkdir -p "$FAKE_ROOT/dev/parameters"
        if [ "$FAKE_MODE" = not_ready ]; then echo N; else echo Y; fi > "$FAKE_ROOT/dev/parameters/ready"
        if [ "$FAKE_MODE" = node_errors ]; then echo 7; else echo 0; fi > "$FAKE_ROOT/dev/parameters/errors"
        ;;
    */droidspaces_compat.ko)
        [ "$FAKE_MODE" != compat_error ] || exit 22
        mkdir -p "$FAKE_ROOT/compat/parameters"
        echo 0 > "$FAKE_ROOT/compat/parameters/balanced"
        ;;
    */droidspaces_netfilter.ko)
        [ "$FAKE_MODE" != netfilter_error ] || exit 23
        mkdir -p "$FAKE_ROOT/netfilter"
        ;;
    *) exit 21;;
esac
''')
        ksud.chmod(0o755)

    def run_loader(self, mode='success', config='ok', release=RELEASE):
        script = '''
MODDIR=$FAKE_ROOT/module
. "$MODDIR/common.sh"
DS_KSUD=$FAKE_ROOT/ksud
DS_COMPATIBLE=$FAKE_ROOT/compatible
DS_BTF=$FAKE_ROOT/btf
DS_BTF_SHA256=$FAKE_BTF_HASH
DS_MODULE_SYS=$FAKE_ROOT/sys
DS_DEV_SYS=$FAKE_ROOT/dev
DS_COMPAT_SYS=$FAKE_ROOT/compat
DS_NETFILTER_SYS=$FAKE_ROOT/netfilter
DS_PROC_NS=$FAKE_ROOT/ns
uname() { if [ "$1" = -m ]; then echo aarch64; else echo "$FAKE_RELEASE"; fi; }
ds_load
'''
        result = subprocess.run([*shlex.split(os.environ.get('LOADER_TEST_SHELL', 'sh')), '-c', script],
                                text=True, capture_output=True,
                                env={**os.environ, 'FAKE_ROOT': str(self.root), 'FAKE_MODE': mode,
                                     'FAKE_CONFIG': config, 'FAKE_BTF_HASH': self.btf_hash,
                                     'FAKE_RELEASE': release})
        self.status = (self.module / 'status.txt').read_text()
        return result

    def calls(self):
        path = self.root / 'calls'
        return path.read_text().splitlines() if path.exists() else []

    def add_docker_payloads(self):
        for name in ('droidspaces_compat', 'droidspaces_netfilter'):
            path = self.module / 'kernel' / (name + '.ko')
            path.write_bytes(('fake ' + name).encode())
            with (self.module / 'SHA256SUMS').open('a') as sums:
                sums.write(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  kernel/{name}.ko\n')

    def test_docker_addons_and_idempotence(self):
        self.add_docker_payloads()
        self.assertEqual(self.run_loader().returncode, 0)
        self.assertIn('PID 引用修复', self.status)
        self.assertIn('droidspaces_compat.ko', self.calls()[2])
        self.assertIn('droidspaces_netfilter.ko', self.calls()[3])
        self.assertEqual(self.run_loader().returncode, 0)
        self.assertEqual(len(self.calls()), 4)

    def test_incomplete_addons_fail_before_namespace_loading(self):
        self.add_docker_payloads()
        (self.module / 'kernel/droidspaces_netfilter.ko').unlink()
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_unhashed_addon_is_not_loaded(self):
        self.add_docker_payloads()
        sums = self.module / 'SHA256SUMS'
        sums.write_text(''.join(line for line in sums.read_text().splitlines(True)
                                if 'droidspaces_compat.ko' not in line))
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_vendor_mismatch_does_not_load_netfilter(self):
        self.add_docker_payloads()
        self.assertNotEqual(self.run_loader(mode='compat_error').returncode, 0)
        self.assertIn('[部分加载]', self.status)
        self.assertIn('droidspaces_compat', self.status)
        self.assertEqual(len(self.calls()), 3)

    def test_netfilter_failure_retries_only_missing_module(self):
        self.add_docker_payloads()
        self.assertNotEqual(self.run_loader(mode='netfilter_error').returncode, 0)
        self.assertIn('[部分加载]', self.status)
        self.assertEqual(self.run_loader().returncode, 0)
        self.assertEqual(len(self.calls()), 5)
        self.assertIn('droidspaces_netfilter', self.calls()[-1])

    def test_success_and_idempotence(self):
        self.assertEqual(self.run_loader().returncode, 0)
        self.assertIn('[已加载]', self.status)
        self.assertEqual(self.status, (self.root / 'description').read_text())
        self.assertEqual(len(self.calls()), 2)
        self.assertIn('droidspaces_ns.ko activate=1', self.calls()[0])
        self.assertIn('droidspaces_devtmpfs.ko', self.calls()[1])
        self.assertEqual(self.run_loader().returncode, 0)
        self.assertEqual(len(self.calls()), 2)

    def test_health_failure_is_partial(self):
        self.assertNotEqual(self.run_loader(mode='health_fail').returncode, 0)
        self.assertIn('[部分加载]', self.status)
        self.assertEqual(len(self.calls()), 2)
        self.assertEqual(self.run_loader().returncode, 0)
        self.assertEqual(len(self.calls()), 2)

    def test_old_active_module_needs_reboot(self):
        self.assertEqual(self.run_loader().returncode, 0)
        (self.root / 'sys/parameters/userns_device_test').unlink()
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertIn('[需重启]', self.status)
        self.assertEqual(len(self.calls()), 2)

    def test_corrupt_health_probe_stops_loading(self):
        (self.module / 'bin/userns-check').write_text('#!/bin/sh\nexit 0\n')
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_btf_mismatch_or_missing(self):
        for data in (b'wrong BTF', None):
            if data is None: (self.root / 'btf').unlink()
            else: (self.root / 'btf').write_bytes(data)
            self.assertNotEqual(self.run_loader().returncode, 0)
            self.assertIn('[不兼容]', self.status)
            self.assertEqual(self.calls(), [])

    def test_wrong_kernel_or_board(self):
        self.assertNotEqual(self.run_loader(release='6.12.99-android16-6-other').returncode, 0)
        (self.root / 'compatible').write_bytes(b'qcom,other\0')
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_corrupt_second_module_stops_before_first(self):
        (self.module / 'kernel/droidspaces_devtmpfs.ko').write_bytes(b'corrupt')
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_ns_failure_does_not_attempt_devtmpfs(self):
        self.assertNotEqual(self.run_loader(mode='ns_error').returncode, 0)
        self.assertIn('[加载失败]', self.status)
        self.assertEqual(len(self.calls()), 1)

    def test_devtmpfs_insmod_failure_is_partial(self):
        self.assertNotEqual(self.run_loader(mode='dev_error').returncode, 0)
        self.assertIn('[部分加载]', self.status)
        self.assertEqual(len(self.calls()), 2)
        self.assertEqual(self.run_loader().returncode, 0)
        self.assertEqual(len(self.calls()), 3)  # Only the missing second module is loaded.

    def test_existing_bad_devtmpfs_is_never_reloaded(self):
        self.assertNotEqual(self.run_loader(mode='node_errors').returncode, 0)
        self.assertIn('[部分加载]', self.status)
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertIn('ready/errors', self.status)
        self.assertEqual(len(self.calls()), 2)

    def test_not_ready_is_not_success(self):
        self.assertNotEqual(self.run_loader(mode='not_ready').returncode, 0)
        self.assertIn('[部分加载]', self.status)

    def test_inactive_existing_ns_stops(self):
        (self.root / 'sys/parameters').mkdir(parents=True)
        (self.root / 'sys/parameters/activate').write_text('N\n')
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_missing_loader_and_fallback_description(self):
        (self.root / 'ksud').unlink()
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertIn('找不到', self.status)
        self.assertIn('description=[加载失败]', (self.module / 'module.prop').read_text())

    def test_fallback_preserves_author_and_one_description(self):
        self.assertEqual(self.run_loader(config='fail').returncode, 0)
        prop = (self.module / 'module.prop').read_text()
        self.assertIn('author=Shikochin', prop)
        self.assertIn('description=[已加载]', prop)
        self.assertEqual(prop.count('description='), 1)


if __name__ == '__main__':
    unittest.main()

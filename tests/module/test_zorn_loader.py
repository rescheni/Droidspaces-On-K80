#!/usr/bin/env python3
"""Exercise the zorn / 6.1.138 boot loader without touching a real kernel.

The loader's device paths, kernel identity and hashes are overridden the same
way the GKI loader test does it: a fake root, a mocked uname and a fake ksud.
"""
import hashlib
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RELEASE = '6.1.138-android14-11-g965475777129-mi'
PROBE_OK = '''#!/bin/sh
printf '%s\\n' 'PASS sysv-create' 'PASS mqueue-open' 'PASS pidns-init-is-pid1'
'''
PROBE_BAD = '''#!/bin/sh
printf '%s\\n' 'PASS sysv-create' 'FAIL mqueue-open'
'''


class ZornLoaderTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.module = self.root / 'module'
        shutil.copytree(ROOT / 'kernelsu-zorn', self.module)
        kernel = self.module / 'kernel'
        kernel.mkdir()
        payload = b'fake module, never passed to the real kernel'
        (kernel / 'droidspaces_ns.ko').write_bytes(payload)
        (self.module / 'bin').mkdir()
        self.probe = self.module / 'bin/nsprobe'
        self.probe.write_text(PROBE_OK)
        self.probe.chmod(0o755)
        (self.module / 'SHA256SUMS').write_text(
            f'{hashlib.sha256(payload).hexdigest()}  kernel/droidspaces_ns.ko\n'
            f'{hashlib.sha256(PROBE_OK.encode()).hexdigest()}  bin/nsprobe\n')
        self.btf = self.root / 'btf'
        self.btf.write_bytes(b'mocked kernel BTF')
        self.ksud = self.root / 'ksud'
        self.ksud.write_text('''#!/bin/sh
if [ "$1" = module ]; then
    [ "$FAKE_CONFIG" != fail ] || exit 1
    printf '%s\\n' "$6" > "$FAKE_ROOT/description"
    exit 0
fi
[ "$1" = insmod ] || exit 2
printf '%s\\n' "$*" >> "$FAKE_ROOT/insmod-calls"
[ "$FAKE_MODE" != error ] || exit 17
[ "$FAKE_MODE" != incomplete ] || exit 0
mkdir -p "$FAKE_ROOT/sys/parameters" "$FAKE_ROOT/ns"
printf 'Y\\n' > "$FAKE_ROOT/sys/parameters/activate"
touch "$FAKE_ROOT/ns/pid" "$FAKE_ROOT/ns/pid_for_children" "$FAKE_ROOT/ns/ipc"
''')
        self.ksud.chmod(0o755)

    def run_loader(self, mode='success', release=RELEASE, config='ok', sha=None,
                   command='ds_load'):
        script = f'''
MODDIR=$FAKE_ROOT/module
. "$MODDIR/common.sh"
DS_KSUD=$FAKE_ROOT/ksud
DS_BTF=$FAKE_ROOT/btf
DS_BTF_SHA256=$FAKE_SHA
DS_MODULE_SYS=$FAKE_ROOT/sys
DS_PROC_NS=$FAKE_ROOT/ns
uname() {{
    case "$1" in
        -m) printf 'aarch64\\n';;
        -r) printf '%s\\n' "$FAKE_RELEASE";;
    esac
}}
{command}
'''
        shell = shlex.split(os.environ.get('LOADER_TEST_SHELL', 'sh'))
        result = subprocess.run([*shell, '-c', script], text=True, capture_output=True,
                                env={**os.environ, 'FAKE_ROOT': str(self.root),
                                     'FAKE_MODE': mode, 'FAKE_RELEASE': release,
                                     'FAKE_CONFIG': config, 'FAKE_SHA': sha or self.btf_sha()})
        self.status = (self.module / 'status.txt').read_text()
        return result

    def btf_sha(self):
        return hashlib.sha256(self.btf.read_bytes()).hexdigest()

    def insmod_calls(self):
        path = self.root / 'insmod-calls'
        return path.read_text().splitlines() if path.exists() else []

    def test_load_and_verify_activation(self):
        result = self.run_loader()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.insmod_calls(),
                         [f'insmod {self.module}/kernel/droidspaces_ns.ko gki=1 activate=1'])
        self.assertIn('[已加载]', (self.root / 'description').read_text())
        self.assertIn('description=[待加载]', (self.module / 'module.prop').read_text())

    def test_repeat_does_not_load_twice(self):
        self.assertEqual(self.run_loader().returncode, 0)
        (self.root / 'insmod-calls').unlink()
        self.assertEqual(self.run_loader().returncode, 0)
        self.assertEqual(self.insmod_calls(), [])
        self.assertIn('本次开机', self.status)

    def test_wrong_release_rejected(self):
        self.assertNotEqual(self.run_loader(release='6.1.166-dirty').returncode, 0)
        self.assertIn('[不支持]', self.status)
        self.assertEqual(self.insmod_calls(), [])

    def test_wrong_btf_fingerprint_rejected(self):
        self.assertNotEqual(self.run_loader(sha='00' * 32).returncode, 0)
        self.assertIn('[不支持]', self.status)
        self.assertEqual(self.insmod_calls(), [])

    def test_corrupt_payload_rejected(self):
        (self.module / 'kernel/droidspaces_ns.ko').write_bytes(b'corrupt')
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertIn('[加载失败]', self.status)
        self.assertEqual(self.insmod_calls(), [])

    def test_missing_probe_rejected(self):
        self.probe.unlink()
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertEqual(self.insmod_calls(), [])

    def test_insmod_failure_visible(self):
        self.assertNotEqual(self.run_loader(mode='error').returncode, 0)
        self.assertIn('返回 17', self.status)

    def test_success_without_activation_is_failure(self):
        self.assertNotEqual(self.run_loader(mode='incomplete').returncode, 0)
        self.assertIn('检查未通过', self.status)

    def test_runtime_probe_failure_reports_partial(self):
        self.assertEqual(self.run_loader(mode='incomplete').returncode, 1)
        self.probe.write_text(PROBE_BAD)
        self.probe.chmod(0o755)
        (self.root / 'ns').mkdir(parents=True, exist_ok=True)
        for name in ('pid', 'pid_for_children', 'ipc'):
            (self.root / 'ns' / name).touch()
        (self.root / 'sys/parameters').mkdir(parents=True, exist_ok=True)
        (self.root / 'sys/parameters/activate').write_text('Y\n')
        self.assertEqual(self.run_loader().returncode, 0)
        self.assertIn('[部分加载]', self.status)

    def test_inactive_existing_module_is_not_reloaded(self):
        (self.root / 'sys').mkdir(parents=True)
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertIn('请重启', self.status)
        self.assertEqual(self.insmod_calls(), [])

    def test_missing_ksud_uses_visible_fallback(self):
        self.ksud.unlink()
        self.assertNotEqual(self.run_loader().returncode, 0)
        self.assertIn('description=[加载失败]', (self.module / 'module.prop').read_text())


if __name__ == '__main__':
    unittest.main()

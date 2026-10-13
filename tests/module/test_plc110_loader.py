#!/usr/bin/env python3
"""Test the manual loader's refusal paths without access to a real kernel."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RELEASE = '6.6.118-android15-8-ge58033dc8ea6-abogki498046332-4k'
BTF_HASH = 'c12101816e70cd7581dd544850758b191c36240f4ce2e966f13f242252ce4484'


class PLC110LoaderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        source = (ROOT / 'scripts/load-plc110-test.sh').read_text()
        for old, new in [('/sys/module/droidspaces_ns', '/sys'),
                         ('/sys/kernel/btf/vmlinux', '/btf'), ('/proc/self/ns/', '/ns/')]:
            source = source.replace(old, str(self.root) + new)
        self.script = self.root / 'load-plc110-test.sh'
        self.script.write_text(source)
        self.payload = self.root / 'droidspaces_ns-6.6.118-plc110-test.ko'
        self.payload.write_bytes(b'fake module: never use a real insmod')
        self.manifest = self.root / 'SHA256SUMS'
        self.manifest.write_text(hashlib.sha256(self.payload.read_bytes()).hexdigest() + '  ' + self.payload.name + '\n')
        (self.root / 'btf').touch()
        self.command('id', 'echo 0')
        self.command('getprop', 'echo "$MODEL"')
        self.command('uname', '[ "$1" = -r ] && echo "$RELEASE" || echo aarch64')
        self.command('dmesg', 'echo "mock kernel diagnostic"')
        self.command('rmmod', 'rm -r "$FAKE_ROOT/sys"')
        self.command('ksud', '''printf '%s\n' "$*" >> "$FAKE_ROOT/calls"
[ "$FAIL_LOAD" != 1 ] || exit 17
mkdir -p "$FAKE_ROOT/sys/parameters" "$FAKE_ROOT/ns"
echo Y > "$FAKE_ROOT/sys/parameters/plc110_test"
case "$*" in *activate=0*) active=N;; *) active=Y;; esac
echo "$active" > "$FAKE_ROOT/sys/parameters/activate"
touch "$FAKE_ROOT/ns/pid" "$FAKE_ROOT/ns/pid_for_children" "$FAKE_ROOT/ns/ipc"
''')
        sha = self.bin / 'sha256sum'
        sha.write_text(f'#!{sys.executable}\n' + '''import hashlib,os,sys
from pathlib import Path
if sys.argv[1] == os.environ['FAKE_ROOT']+'/btf':
    print(os.environ['BTF_HASH']+'  '+sys.argv[1])
elif sys.argv[1] == '-c':
    digest,name=Path(sys.argv[2]).read_text().split()
    sys.exit(0 if hashlib.sha256(Path(name).read_bytes()).hexdigest()==digest else 1)
else:
    sys.exit(2)
''')
        sha.chmod(0o755)

    def command(self, name, body):
        path = self.bin / name
        path.write_text('#!/bin/sh\n' + body + '\n')
        path.chmod(0o755)

    def run_loader(self, mode='activate', **changes):
        env = {**os.environ, 'FAKE_ROOT': str(self.root), 'MODEL': 'PLC110',
               'RELEASE': RELEASE, 'BTF_HASH': BTF_HASH, 'FAIL_LOAD': '0',
               'KSUD': str(self.bin / 'ksud'), 'PATH': str(self.bin) + ':' + os.environ['PATH'], **changes}
        return subprocess.run(['sh', str(self.script), mode], env=env, capture_output=True, text=True)

    def assert_refused(self, **changes):
        result = self.run_loader(**changes)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse((self.root / 'calls').exists())

    def test_model(self):
        self.assert_refused(MODEL='PLZ110')

    def test_release(self):
        self.assert_refused(RELEASE='6.6.other')

    def test_btf(self):
        self.assert_refused(BTF_HASH='0' * 64)

    def test_payload_corruption(self):
        self.payload.write_bytes(b'corrupted')
        self.assert_refused()

    def test_missing_manifest(self):
        self.manifest.unlink()
        self.assert_refused()

    def test_existing_module(self):
        (self.root / 'sys').mkdir()
        self.assert_refused()

    def test_success(self):
        result = self.run_loader()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('plc110_test=1 activate=1', (self.root / 'calls').read_text())
        self.assertIn('LOADED:', result.stdout)

    def test_selftest_unloads(self):
        result = self.run_loader(mode='selftest')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.root / 'sys').exists())
        self.assertIn('plc110_test=1 activate=0', (self.root / 'calls').read_text())

    def test_failure_keeps_diagnostics(self):
        result = self.run_loader(FAIL_LOAD='1')
        self.assertNotEqual(result.returncode, 0)
        log = (self.root / 'load.log').read_text()
        self.assertIn('ksud returned 17', log)
        self.assertIn('mock kernel diagnostic', log)


if __name__ == '__main__':
    unittest.main()

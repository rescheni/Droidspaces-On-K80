#!/usr/bin/env python3
"""Exercise ABI constraint failures using small synthetic BTF inputs."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def btf(offset=0, signed=True):
    strings = b'\0int\0sample\0field\0fn\0arg\0'
    def typ(name, kind, value, words=(), count=0):
        return struct.pack('<III', name, kind << 24 | count, value) + struct.pack('<'+'I'*len(words), *words)
    types = typ(1, 1, 4, ((1 << 24 if signed else 0) | 32,))
    types += typ(5, 4, 4, (12, 1, offset), 1)
    types += typ(0, 13, 1, (21, 1), 1)
    types += typ(18, 12, 3)
    header = struct.pack('<HBBIIIII', 0xeb9f, 1, 0, 24, 0, len(types), len(types), len(strings))
    return header + types + strings


class ABICheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.bin = cls.root / 'abi-check'
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(ROOT/'src/abi/check.c'),'-o',str(cls.bin)],check=True)
        spec = importlib.util.spec_from_file_location('constraints', ROOT/'scripts/abi/make-constraints.py')
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_check(self, data, constraints=None):
        kernel = self.root / 'kernel.btf'
        expected = self.root / 'expected'
        kernel.write_bytes(btf())
        baseline = self.module.btf.BTF(kernel)
        signature = self.module.prototype_hash(baseline.types, 3)
        expected.write_text(constraints or f'DSABI1\nS sample 4 1\nF field 0 32\nP fn {signature:016x}\n')
        kernel.write_bytes(data)
        return subprocess.run([str(self.bin),'--offline',str(expected),str(kernel)],capture_output=True,text=True)

    def test_match(self):
        self.assertEqual(self.run_check(btf()).returncode,0)

    def test_changed_field_offset(self):
        r=self.run_check(btf(offset=8));self.assertNotEqual(r.returncode,0);self.assertIn('member differs',r.stderr)

    def test_changed_function_signedness(self):
        r=self.run_check(btf(signed=False));self.assertNotEqual(r.returncode,0);self.assertIn('prototype differs',r.stderr)

    def test_truncated_btf(self):
        self.assertNotEqual(self.run_check(btf()[:-8]).returncode,0)

    def test_missing_type(self):
        self.assertNotEqual(self.run_check(btf(),'DSABI1\nS absent 4 0\n').returncode,0)

    def test_empty_constraints(self):
        self.assertNotEqual(self.run_check(btf(),'DSABI1\n').returncode,0)


if __name__=='__main__': unittest.main()

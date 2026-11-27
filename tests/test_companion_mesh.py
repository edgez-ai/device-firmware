#!/usr/bin/env python3
"""Run SDK-generated mesh control vectors through the production companion codec."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class CompanionMesh(unittest.TestCase):
    def test_sdk_protocol(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / 'companion-mesh'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I' + str(ROOT / 'firmware/src'),
                str(ROOT / 'firmware/src/companion_protocol.c'),
                str(ROOT / 'firmware/src/companion_mesh_protocol.c'),
                str(ROOT / 'tests/native/companion_mesh.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
if __name__ == '__main__': unittest.main()

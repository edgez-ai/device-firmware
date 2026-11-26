#!/usr/bin/env python3
"""Build the production companion codec and session policy with sanitizers."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class Companion(unittest.TestCase):
    def test_lifecycle_and_protocol(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'companion'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I' + str(ROOT / 'firmware/src'),
                str(ROOT / 'firmware/src/companion_protocol.c'),
                str(ROOT / 'tests/native/companion.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
if __name__ == '__main__': unittest.main()

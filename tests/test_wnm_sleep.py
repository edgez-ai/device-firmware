#!/usr/bin/env python3
"""Verify timed WNM control, preserving association and restoring IP TX on errors."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class WnmSleep(unittest.TestCase):
    def test_sleep_lifecycle(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp)/'wnm-sleep'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-DLWM2M_CLIENT_MODE',
                '-I'+str(ROOT/'tests/native/wnm_platform'), '-I'+str(ROOT/'tests/native'),
                '-I'+str(ROOT/'firmware/src'), '-I'+str(ROOT/'third_party/wakaama/include'),
                str(ROOT/'firmware/src/halow_sleep.c'),str(ROOT/'firmware/src/halow_wnm.c'),
                str(ROOT/'tests/native/wnm_sleep.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
if __name__ == '__main__': unittest.main()

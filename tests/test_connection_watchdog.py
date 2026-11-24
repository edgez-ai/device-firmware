#!/usr/bin/env python3
"""Verify general connection recovery, healthy ACKs and paused WNM deadlines."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class ConnectionWatchdog(unittest.TestCase):
    def test_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'watchdog'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-DLWM2M_CLIENT_MODE',
                '-DCONFIG_EZ_DEV54_CONNECTION_PROBE_SECONDS=60','-DCONFIG_EZ_DEV54_CONNECTION_RESPONSE_SECONDS=45',
                '-I'+str(ROOT/'tests/native'),'-I'+str(ROOT/'firmware/src'),
                '-I'+str(ROOT/'third_party/wakaama/include'),str(ROOT/'firmware/src/halow_connection.c'),
                str(ROOT/'tests/native/connection_watchdog.c'),'-o',str(binary)],check=True)
            for case in range(1,9):
                with self.subTest(case=case):subprocess.run([str(binary),str(case)],check=True)
if __name__=='__main__':unittest.main()

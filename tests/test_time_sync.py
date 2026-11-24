#!/usr/bin/env python3
"""Validate registration ACK time metadata using the real Wakaama CoAP parser."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class TimeSync(unittest.TestCase):
    def test_registration_time(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp)/'time-sync'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-DLWM2M_CLIENT_MODE','-DLWM2M_LITTLE_ENDIAN',
                '-I'+str(ROOT/'tests/native'),'-I'+str(ROOT/'firmware/src'),'-I'+str(ROOT/'third_party/wakaama/include'),
                '-I'+str(ROOT/'third_party/wakaama/coap'),
                str(ROOT/'firmware/src/halow_time_sync.c'), str(ROOT/'tests/native/time_sync.c'),
                str(ROOT/'third_party/wakaama/coap/er-coap-13/er-coap-13.c'), '-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
if __name__ == '__main__':unittest.main()

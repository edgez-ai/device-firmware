#!/usr/bin/env python3
"""Validate actual GPS object and NMEA parsing under ASan/UBSan."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Gps(unittest.TestCase):
    def test_uart_power_boundary(self):
        with tempfile.TemporaryDirectory() as folder:
            tmp = Path(folder)
            source = tmp/'gps_hw.c'
            original = (ROOT/'firmware/src/gps_hw.c').read_text()
            source.write_text(re.sub(r'^#include [<"]zephyr/[^>"\n]+[>"]\n', '', original, flags=re.M))
            binary = tmp/'gps-hw'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-DCONFIG_EZ_DEV54_UART_GPS=1', '-DRADAR_HALOW_LIGHT',
                            '-I'+str(ROOT/'tests/native'), '-I'+str(ROOT/'firmware/src'),
                            '-include', str(ROOT/'tests/native/gps_hw_shim.h'),
                            str(source), str(ROOT/'tests/native/gps_hw.c'), '-o', str(binary)], check=True)
            for case in range(1, 6):
                with self.subTest(case=case):
                    subprocess.run([str(binary), str(case)], check=True)

    def test_on_demand_lifecycle(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / 'gps'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-DLWM2M_CLIENT_MODE',
                            '-DCONFIG_EZ_DEV54_UART_GPS=1', '-DCONFIG_EZ_DEV54_GPS_FIX_TIMEOUT_SECONDS=120',
                            '-I'+str(ROOT/'tests/native'), '-I'+str(ROOT/'firmware/src'),
                            '-I'+str(ROOT/'third_party/wakaama/include'),
                            str(ROOT/'firmware/src/halow_gps.c'), str(ROOT/'firmware/src/gps_nmea.c'),
                            str(ROOT/'tests/native/gps.c'), '-lm', '-o', str(binary)], check=True)
            for args in ([], ['hold-cancel'], ['hold-error']):
                with self.subTest(args=args):
                    subprocess.run([str(binary), *args], check=True)


if __name__ == '__main__':
    unittest.main()

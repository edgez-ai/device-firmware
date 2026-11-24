#!/usr/bin/env python3
"""Run real sensor object/Observe callbacks against a fake board, without radio/SWD."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class Lifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name)/'sensors'
        subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-DLWM2M_CLIENT_MODE',
                        '-DCONFIG_EZ_CAM_IMU_OBSERVE_INTERVAL_MS=1000',
                        '-I'+str(ROOT/'tests/native'), '-I'+str(ROOT/'firmware/src'),
                        '-I'+str(ROOT/'third_party/wakaama/include'),
                        str(ROOT/'tests/native/sensors_lifecycle.c'),
                        str(ROOT/'firmware/src/halow_sensors.c'), str(ROOT/'firmware/src/halow_device.c'), '-o', str(cls.binary)], check=True)
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()
    def test_read_observe_and_cleanup(self):
        scenarios = ['Read without sampling loop', 'metadata/discover/invalid path',
                     'attribute-only watcher stays asleep', 'changes and last cancellation',
                     'independent accel/gyro watchers', 'registration/link loss',
                     'failed sample invalidates cache', 'voltage on demand',
                     'units Observe stays asleep', 'whole-object Observe', 'battery voltage policy',
                     'real Device metadata and absent unsupported resources', 'clock and atomic writes',
                     'reboot/reset dispatch', 'Device aggregate Read samples ADC once', 'local UTC ticking and Current Time Observe lifecycle']
        for number, name in enumerate(scenarios, 1):
            with self.subTest(name=name):
                result = subprocess.run([str(self.binary), str(number)], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), 'OK')
if __name__ == '__main__': unittest.main()

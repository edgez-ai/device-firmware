#!/usr/bin/env python3
"""Exercise actual GPIO parking source with simulated nRF registers and SPI PM."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WnmPins(unittest.TestCase):
    def test_parking_and_restore(self):
        with tempfile.TemporaryDirectory() as folder:
            tmp = Path(folder)
            source = tmp / 'halow_wnm_pins.c'
            original = (ROOT / 'firmware/src/halow_wnm_pins.c').read_text()
            source.write_text(re.sub(r'^#include [<"](?:zephyr/|hal/)[^>"\n]+[>"]\n', '', original, flags=re.M))
            binary = tmp / 'wnm-pins'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined',
                            '-I'+str(ROOT/'firmware/src'), '-I'+str(ROOT/'tests/native'),
                            '-include', str(ROOT/'tests/native/wnm_pins_shim.h'),
                            str(source), str(ROOT/'tests/native/wnm_pins.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

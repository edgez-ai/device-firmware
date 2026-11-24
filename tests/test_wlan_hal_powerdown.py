#!/usr/bin/env python3
"""Verify active-low reset polarity in the actual pinned Zephyr WLAN HAL."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'firmware/platforms/mm-iot-zephyr/drivers/wifi/morsemicro/shims/mmhal_wlan.c'
class HalPowerdown(unittest.TestCase):
    def test_reset_held_during_powerdown(self):
        with tempfile.TemporaryDirectory() as folder:
            tmp=Path(folder);source=tmp/'mmhal_wlan.c';shutil.copyfile(SOURCE,source)
            for name in re.findall(r'^#include [<"]([^>"]+)[>"]',SOURCE.read_text(),re.M):
                header=tmp/name;header.parent.mkdir(parents=True,exist_ok=True);header.write_text('')
            binary=tmp/'hal-powerdown'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                '-fsanitize=address,undefined','-DCONFIG_WIFI_MORSEMICRO_POWERSAVE=1',
                '-I'+str(tmp),'-I'+str(ROOT/'tests/native'),'-include',str(ROOT/'tests/native/wlan_hal_shim.h'),
                str(source),str(ROOT/'tests/native/wlan_hal_powerdown.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
if __name__=='__main__':unittest.main()

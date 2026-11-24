#!/usr/bin/env python3
"""Compile the pinned SDK WNM source/FSM against deterministic hardware stubs."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
SDK=ROOT/'firmware/platforms/mm-iot-zephyr/submodules/mm-iot-sdk/framework/morselib/src/umac/wnm_sleep'
class SdkWake(unittest.TestCase):
    def test_real_sdk_exit(self):
        with tempfile.TemporaryDirectory() as folder:
            tmp=Path(folder)
            # Copy the exact SDK sources so quoted dependency includes resolve to
            # stubs, without editing the SDK's generated FSM or production headers.
            for name in ('umac_wnm_sleep.c','wnm_sleep_fsm.h','wnm_sleep_fsm_def.h'):
                shutil.copyfile(SDK/name,tmp/name)
            for name in re.findall(r'^#include "([^"]+)"',(SDK/'umac_wnm_sleep.c').read_text(),re.M):
                target=tmp/name
                if target.exists(): continue
                target.parent.mkdir(parents=True,exist_ok=True);target.write_text('')
            binary=tmp/'sdk-wnm'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter',
                '-fsanitize=address,undefined','-I'+str(tmp),'-I'+str(ROOT/'tests/native'),
                '-include',str(ROOT/'tests/native/wnm_sdk_shim.h'),str(tmp/'umac_wnm_sleep.c'),
                str(ROOT/'tests/native/wnm_sdk_exit.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
if __name__=='__main__':unittest.main()

from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class HardwareMemory(unittest.TestCase):
    def test_selected_workspace_and_allocation_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            source = (ROOT/'firmware/src/hardware_memory.c').read_text()
            (tmp/'hardware_memory_under_test.c').write_text(re.sub(r'^#include <zephyr/[^>]+>\n', '', source, flags=re.M))
            binary = tmp/'memory'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-DCONFIG_RADAR_UART_RX_BUFFER_SIZE=1024','-I'+str(tmp),'-I'+str(ROOT/'firmware/src'),
                str(ROOT/'tests/native/hardware_memory.c'),'-o',str(binary)],check=True)
            for scenario in range(6):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary),str(scenario)],check=True)

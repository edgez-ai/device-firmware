from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class HardwareConfig(unittest.TestCase):
    def test_persistent_runtime_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            source = (ROOT/'firmware/src/hardware_config.c').read_text()
            (tmp/'hardware_config_under_test.c').write_text(re.sub(r'^#include <zephyr/[^>]+>\n', '', source, flags=re.M))
            binary = tmp/'hardware'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-I'+str(tmp),'-I'+str(ROOT/'tests/native'),'-I'+str(ROOT/'firmware/src'),
                str(ROOT/'tests/native/hardware_config.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_lwm2m_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory)/'object'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-DLWM2M_CLIENT_MODE','-I'+str(ROOT/'firmware/src'),'-I'+str(ROOT/'third_party/wakaama/include'),
                str(ROOT/'firmware/src/halow_hardware.c'),str(ROOT/'tests/native/hardware_object.c'),
                '-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_shared_uart_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            source = (ROOT/'firmware/src/hardware_uart.c').read_text()
            (tmp/'hardware_uart_under_test.c').write_text(re.sub(r'^#include <(?:zephyr|hal)/[^>]+>\n', '', source, flags=re.M))
            binary = tmp/'uart'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-I'+str(tmp),'-I'+str(ROOT/'firmware/src'),
                str(ROOT/'tests/native/hardware_uart.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

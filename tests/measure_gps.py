#!/usr/bin/env python3
"""Read RAM-only GPS timing diagnostics over SWD without resetting/flashing."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--probe', required=True)
parser.add_argument('--elf', type=Path, default=ROOT/'firmware/.pio/build/ez-dev54/firmware.elf')
parser.add_argument('--nm', default='arm-none-eabi-nm')
parser.add_argument('--jlink', default='JLinkExe')
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--tag', default='snapshot')
args = parser.parse_args()
if not re.fullmatch(r'[A-Za-z0-9_-]+', args.tag): parser.error('tag must be a plain filename stem')
args.out.mkdir(mode=0o700, parents=True, exist_ok=True)
args.out.chmod(0o700)
symbols = {}
for line in subprocess.check_output([args.nm, '-S', str(args.elf)], text=True).splitlines():
    fields = line.split()
    if len(fields) == 4 and fields[3] in ('gps_uart_diagnostics', 'gps_acquisition_diagnostics'):
        symbols[fields[3]] = (int(fields[0], 16), int(fields[1], 16))
assert len(symbols) == 2, 'Build the instrumented firmware first'
script = args.out / (args.tag+'.jlink')
commands = []
for name, (address, length) in symbols.items():
    path = args.out/(args.tag+'-'+name+'.bin')
    commands.append(f'savebin "{path}", {address:#x}, {length:#x}')
commands.append('exit')
script.write_text('\n'.join(commands)+'\n')
result = subprocess.run([args.jlink, '-device', 'NRF54L15_M33', '-if', 'SWD', '-speed', '4000',
                         '-SelectEmuBySN', args.probe, '-autoconnect', '1', '-CommanderScript', str(script)],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=True)
(args.out/(args.tag+'.log')).write_text(result.stdout)
def binary(name): return (args.out/(args.tag+'-'+name+'.bin')).read_bytes()
uart_names = ['starts', 'started_ms', 'first_rx_ms', 'last_rx_ms', 'received_bytes', 'dropped_bytes', 'stopped_ms']
uart = dict(zip(uart_names, struct.unpack('<7I', binary('gps_uart_diagnostics'))))
raw = binary('gps_acquisition_diagnostics')
acq_names = ['sequence', 'requested_ms', 'first_step_ms', 'first_line_ms', 'fixed_ms', 'finished_ms',
             'lines', 'rmc_lines', 'active_rmc_lines', 'rejected_rmc_lines', 'state', 'result']
acquisition = dict(zip(acq_names, struct.unpack('<12I', raw[:48])))
if acquisition['result'] >= 0x80000000: acquisition['result'] -= 0x100000000
acquisition['last_line'] = raw[48:176].split(b'\0')[0].decode(errors='replace')
acquisition['last_rmc'] = raw[176:304].split(b'\0')[0].decode(errors='replace')
output = {'uart': uart, 'acquisition': acquisition}
for name, value in [('power_to_first_byte_ms', uart['first_rx_ms']),
                    ('power_to_first_line_ms', acquisition['first_line_ms']),
                    ('power_to_fix_ms', acquisition['fixed_ms'])]:
    output[name] = None if value == 0xffffffff else (value-uart['started_ms']) & 0xffffffff
(args.out/(args.tag+'.json')).write_text(json.dumps(output, indent=2)+'\n')
for path in args.out.iterdir():
    if path.is_file(): path.chmod(0o600)
# Sentence contents remain in the private artifact, avoiding accidental location
# disclosure when copying timing summaries into bug reports.
print(json.dumps({key: value for key, value in output.items() if key not in ('uart', 'acquisition')} |
                 {'state': acquisition['state'], 'sequence': acquisition['sequence'], 'lines': acquisition['lines'],
                  'rmc_lines': acquisition['rmc_lines'], 'active_rmc_lines': acquisition['active_rmc_lines'],
                  'received_bytes': uart['received_bytes'], 'dropped_bytes': uart['dropped_bytes']}))

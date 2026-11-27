#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Isolate mesh HostAP crypto hooks from the normal station's mmint ABI.

Release archives stay untouched; matching definitions and references in the
build-local copies are renamed together using the toolchain's objcopy.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--nm', required=True)
parser.add_argument('--objcopy', required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('archives', nargs='+', type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
symbols = set()
for archive in args.archives:
    output = subprocess.check_output([args.nm, '-g', '--defined-only', str(archive)], text=True)
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1].startswith('mmint_'):
            symbols.add(parts[-1])
remap = args.output / 'crypto-symbols.txt'
remap.write_text(''.join(f'{s} ez_companion_{s}\n' for s in sorted(symbols)))
for archive in args.archives:
    destination = args.output / archive.name
    shutil.copyfile(archive, destination)
    subprocess.run([args.objcopy, '--redefine-syms=' + str(remap), str(destination)], check=True)

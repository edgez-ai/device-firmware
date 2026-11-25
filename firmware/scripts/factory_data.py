#!/usr/bin/env python3
"""Create, inspect, update and flash the tracker-compatible nRF54 factory page.

Factory images contain private identity keys. Store them and BLE QR payloads
outside Git. All output files are created with owner-only permissions.
"""
import argparse
import datetime
import hashlib
import hmac
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib

ADDRESS, PAGE_SIZE = 0x165000, 4096
FORMAT = '<IHH33s32s32s3s33sB65sI'
RECORD_SIZE = struct.calcsize(FORMAT)
# Versioned KDF domain, retained so the product rename does not change PoPs.
LABEL = b'EZ-CAM01 BLE provisioning PoP v1'
HARDWARE = ('base', 'camera', 'radar', 'gps')
HARDWARE_OFFSET = 256
HARDWARE_MAGIC = 0x57485a45
MOTION_OFFSET = 272
MOTION_MAGIC = 0x4d505a45


def secret_write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # Never follow symlinks or silently overwrite a factory backup.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)


def text_field(value, maximum, name):
    raw = value.encode('utf-8')
    if b'\0' in raw or len(raw) > maximum:
        raise ValueError(f'{name} must be at most {maximum} UTF-8 bytes without NUL')
    return raw


def validate(record):
    if not record['serial']:
        raise ValueError('Serial must not be empty')
    text_field(record['serial'], 32, 'Serial')
    country = record['country']
    if len(country) != 2 or not country.isascii() or not country.isalpha() or not country.isupper():
        raise ValueError('Country must contain two uppercase ASCII letters')
    if not 1 <= record['channel'] <= 255:
        raise ValueError('Channel must be between 1 and 255')
    text_field(record['ssid'], 32, 'SSID')
    password = text_field(record['password'], 63, 'WPA3 password')
    if bool(record['ssid']) != bool(password) or (password and len(password) < 8):
        raise ValueError('Factory Wi-Fi must be empty or include SSID and an 8–63-byte password')
    threshold = record.get('pose_delta_mg')
    if threshold is not None and (type(threshold) is not int or not 100 <= threshold <= 1000):
        raise ValueError('Pose delta must be an integer between 100 and 1000 mg')
    angle = record.get('pose_interrupt_deg', 60)
    if type(angle) is not int or angle not in (50, 60, 70, 80):
        raise ValueError('Pose interrupt angle must be 50, 60, 70 or 80 degrees')
    for name in ('public_key', 'private_key'):
        if len(record[name]) != 32 or not any(record[name]):
            raise ValueError(f'Invalid {name}')


def decode(data):
    if len(data) != PAGE_SIZE:
        raise ValueError('Expected one exact 4096-byte factory page')
    magic, version, size, serial, public, private, country, ssid, channel, password, crc = struct.unpack(FORMAT, data[:RECORD_SIZE])
    if (magic, version, size) != (0x455a4631, 2, RECORD_SIZE) or zlib.crc32(data[:RECORD_SIZE-4]) != crc:
        raise ValueError('Factory record header or CRC is invalid')
    def string(raw):
        if b'\0' not in raw:
            raise ValueError('Unterminated factory string')
        return raw.split(b'\0', 1)[0].decode('utf-8')
    record = dict(serial=string(serial), country=string(country), ssid=string(ssid),
                  password=string(password), channel=channel, public_key=public, private_key=private)
    validate(record)
    extension = data[HARDWARE_OFFSET:HARDWARE_OFFSET+16]
    magic, ver, size, mode, reserved, crc = struct.unpack('<IHHB3sI', extension)
    if (magic, ver, size) == (HARDWARE_MAGIC, 1, 16) and mode < len(HARDWARE) and reserved == bytes(3) and zlib.crc32(extension[:12]) == crc:
        record['hardware'] = HARDWARE[mode]
    extension = data[MOTION_OFFSET:MOTION_OFFSET+16]
    magic, ver, size, mg, angle, reserved, crc = struct.unpack('<IHHHBBI', extension)
    if (magic, ver, size) == (MOTION_MAGIC, 2, 16) and 100 <= mg <= 1000 and angle in (50, 60, 70, 80) and reserved == 0 and zlib.crc32(extension[:12]) == crc:
        record['pose_delta_mg'] = mg
        record['pose_interrupt_deg'] = angle
    return record


def encode(record, original=None):
    validate(record)
    raw = struct.pack(FORMAT, 0x455a4631, 2, RECORD_SIZE, record['serial'].encode(),
                      record['public_key'], record['private_key'], record['country'].encode(),
                      record['ssid'].encode(), record['channel'], record['password'].encode(), 0)
    raw = raw[:-4] + struct.pack('<I', zlib.crc32(raw[:-4]))
    page = bytearray(raw + (original[RECORD_SIZE:] if original is not None else b'\xff' * (PAGE_SIZE-RECORD_SIZE)))
    if 'hardware' in record:
        old = page[HARDWARE_OFFSET:HARDWARE_OFFSET+16]
        if old not in (bytes(16), b'\xff'*16) and struct.unpack('<I', old[:4])[0] != HARDWARE_MAGIC:
            raise ValueError('Hardware extension overlaps unknown factory data')
        if record['hardware'] is None:
            extension = b'\xff'*16
        else:
            prefix = struct.pack('<IHHB3s', HARDWARE_MAGIC, 1, 16, HARDWARE.index(record['hardware']), bytes(3))
            extension = prefix + struct.pack('<I', zlib.crc32(prefix))
        page[HARDWARE_OFFSET:HARDWARE_OFFSET+16] = extension
    if 'pose_delta_mg' in record or 'pose_interrupt_deg' in record:
        old = page[MOTION_OFFSET:MOTION_OFFSET+16]
        if old not in (bytes(16), b'\xff'*16) and struct.unpack('<I', old[:4])[0] != MOTION_MAGIC:
            raise ValueError('Motion extension overlaps unknown factory data')
        if 'pose_delta_mg' in record and record['pose_delta_mg'] is None:
            extension = b'\xff'*16
        else:
            prefix = struct.pack('<IHHHBB', MOTION_MAGIC, 2, 16, record.get('pose_delta_mg', 500), record.get('pose_interrupt_deg', 60), 0)
            extension = prefix + struct.pack('<I', zlib.crc32(prefix))
        page[MOTION_OFFSET:MOTION_OFFSET+16] = extension
    return bytes(page)


def ble_qr(record):
    return dict(ver='v1', name='PROV_CAM' + hashlib.sha256(record['public_key']).hexdigest()[:6],
                pop=hmac.new(record['private_key'], LABEL, hashlib.sha256).hexdigest()[:32], transport='ble')


def summary(record):
    # Never expose credentials, PoP or private key on stdout.
    return dict(serial=record['serial'], country=record['country'], channel=record['channel'],
                factory_wifi=bool(record['ssid']), pose_delta_mg=record.get('pose_delta_mg') or 500, pose_interrupt_deg=record.get('pose_interrupt_deg', 60), hardware=record.get('hardware', 'base (default)'), public_key_sha256=hashlib.sha256(record['public_key']).hexdigest(),
                ble_name=ble_qr(record)['name'])


def generate_identity():
    private_der = subprocess.check_output(['openssl', 'genpkey', '-algorithm', 'X25519', '-outform', 'DER'], stderr=subprocess.DEVNULL)
    public_der = subprocess.check_output(['openssl', 'pkey', '-inform', 'DER', '-pubout', '-outform', 'DER'], input=private_der, stderr=subprocess.DEVNULL)
    return private_der[-32:], public_der[-32:]


def run_jlink(script, serial, log):
    executable = shutil.which('JLinkExe') or '/usr/local/bin/JLinkExe'
    with tempfile.TemporaryDirectory(prefix='ezdev54-factory-') as folder:
        commands = Path(folder) / 'factory.jlink'
        commands.write_text(script)
        command = [executable, '-device', 'NRF54L15_M33', '-if', 'SWD', '-speed', '4000',
                   '-AutoConnect', '1', '-ExitOnError', '1', '-SelectEmuBySN', serial, '-CommandFile', str(commands)]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        secret_write(log, result.stdout)
        output = result.stdout.decode(errors='replace')
        if result.returncode or 'Cannot connect' in output or 'Error while' in output or 'Failed' in output:
            raise RuntimeError(f'J-Link failed; inspect {log}')
        return output


def flash(args):
    image = args.image.resolve()
    record = decode(image.read_bytes())
    if not args.serial.isdigit():
        raise ValueError('Probe serial must be numeric')
    if any(c in str(image) for c in '\r\n"'):
        raise ValueError('Unsupported character in factory path')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    folder = args.backup_dir.resolve(); folder.mkdir(parents=True, exist_ok=True)
    if any(c in str(folder) for c in '\r\n"'):
        raise ValueError('Unsupported character in backup path')
    backup = folder / f'factory-{stamp}-before.bin'
    # J-Link opens savebin itself; the private temporary directory protects it.
    with tempfile.TemporaryDirectory(prefix='ezdev54-factory-read-') as temp:
        saved = Path(temp) / 'before.bin'
        run_jlink(f'h\nsavebin "{saved}" 0x{ADDRESS:x} 0x{PAGE_SIZE:x}\ng\nexit\n', args.serial, folder / f'{stamp}-read.log')
        before = saved.read_bytes()
        secret_write(backup, before)
    if len(before) != PAGE_SIZE:
        raise ValueError('Incomplete device backup; refusing to write')
    if before != b'\xff' * PAGE_SIZE:
        previous = decode(before)  # Fail closed on unknown layout/corruption.
        if not args.replace:
            raise ValueError('Existing valid factory data; use --replace for an explicit update')
        if any(previous[k] != record[k] for k in ('serial', 'public_key', 'private_key')):
            raise ValueError('Identity differs; refusing to replace serial or identity keys')
    log = folder / f'{stamp}-flash.log'
    output = run_jlink(f'r\nloadbin "{image}" 0x{ADDRESS:x}\nverifybin "{image}" 0x{ADDRESS:x}\nr\ng\nexit\n', args.serial, log)
    if 'Verify successful' not in output:
        raise RuntimeError(f'Flash did not report verification success; inspect {log}')
    print(f'Factory page programmed and verified at 0x{ADDRESS:x}; backup: {backup}')
    print(json.dumps(summary(record), indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('create', 'update'):
        cmd = commands.add_parser(name)
        if name == 'create':
            cmd.add_argument('--serial', required=True)
            cmd.add_argument('--country', required=True)
            cmd.add_argument('--channel', type=int, default=1)
        else:
            cmd.add_argument('--input', type=Path, required=True)
            cmd.add_argument('--country')
            cmd.add_argument('--channel', type=int)
        hardware = cmd.add_mutually_exclusive_group()
        hardware.add_argument('--hardware', choices=HARDWARE)
        hardware.add_argument('--clear-hardware', action='store_true')
        motion = cmd.add_mutually_exclusive_group()
        motion.add_argument('--pose-delta-mg', type=int, help='100–1000 mg; default 500 mg when absent')
        motion.add_argument('--clear-pose-threshold', action='store_true')
        cmd.add_argument('--pose-interrupt-deg', type=int, choices=(50,60,70,80))
        cmd.add_argument('--ssid')
        cmd.add_argument('--password-file', type=Path, help='UTF-8 WPA3 password file; never place secrets on the command line')
        cmd.add_argument('--clear-wifi', action='store_true')
        cmd.add_argument('--output', type=Path, required=True)
    for name in ('inspect', 'qr', 'flash'):
        cmd = commands.add_parser(name)
        cmd.add_argument('image', type=Path)
        if name == 'qr': cmd.add_argument('--output', type=Path, required=True, help='Private JSON payload accepted by ESP BLE Prov QR scanner')
        if name == 'flash':
            cmd.add_argument('--serial', required=True, help='J-Link probe serial')
            cmd.add_argument('--replace', action='store_true')
            cmd.add_argument('--backup-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'flash': return flash(args)
        if args.command in ('inspect', 'qr'):
            record = decode(args.image.read_bytes())
            if args.command == 'inspect': print(json.dumps(summary(record), indent=2))
            else:
                secret_write(args.output, (json.dumps(ble_qr(record)) + '\n').encode())
                print(f'Private BLE QR payload saved: {args.output}')
            return
        original = None
        if args.command == 'create':
            private, public = generate_identity()
            record = dict(serial=args.serial, country=args.country, channel=args.channel,
                          ssid='', password='', private_key=private, public_key=public)
        else:
            original = args.input.read_bytes(); record = decode(original)
            if args.country: record['country'] = args.country
            if args.channel is not None: record['channel'] = args.channel
        if args.hardware is not None: record['hardware'] = args.hardware
        if args.clear_hardware: record['hardware'] = None
        if args.pose_delta_mg is not None: record['pose_delta_mg'] = args.pose_delta_mg
        if args.clear_pose_threshold:
            record['pose_delta_mg'] = None
            record.pop('pose_interrupt_deg', None)
        if args.pose_interrupt_deg is not None: record['pose_interrupt_deg'] = args.pose_interrupt_deg
        if args.clear_wifi:
            if args.ssid is not None or args.password_file is not None: raise ValueError('--clear-wifi conflicts with Wi-Fi options')
            record['ssid'] = record['password'] = ''
        else:
            if args.ssid is not None: record['ssid'] = args.ssid
            if args.password_file is not None: record['password'] = args.password_file.read_text().rstrip('\r\n')
        secret_write(args.output, encode(record, original))
        print(f'Private factory image saved: {args.output}')
        print(json.dumps(summary(record), indent=2))
    except (ValueError, OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Factory operation failed: {error}\n')


if __name__ == '__main__':
    main()

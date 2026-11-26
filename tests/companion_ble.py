#!/usr/bin/env python3
"""Exercise unprovisioned companion ownership on real BLE hardware.

Does not write Wi-Fi, factory identity or settings. --hold-seconds 185 verifies
that a live link survives the normal three-minute provisioning deadline.
"""
import argparse
import asyncio
import json
import struct
import time
from bleak import BleakClient, BleakScanner
PROVISIONING = '1775244d-6b43-439b-877c-060f2d9bed07'
VERSION = '1775ff53-6b43-439b-877c-060f2d9bed07'
ENDPOINTS = [f'1775ff{suffix}-6b43-439b-877c-060f2d9bed07' for suffix in ('51', '52', '50', '53', '54')]
SERVICE = '0000fff0-0000-1000-8000-00805f9b34fb'
RX = '0000fff1-0000-1000-8000-00805f9b34fb'
TX = '0000fff2-0000-1000-8000-00805f9b34fb'

async def find(name):
    device = await BleakScanner.find_device_by_filter(lambda d, a: (a.local_name or d.name) == name, timeout=15)
    if device is None: raise RuntimeError(f'{name} is not advertising')
    return device

async def main(args):
    if args.expect_provisioned:
        device = await BleakScanner.find_device_by_filter(lambda d, a: (a.local_name or d.name) == args.name, timeout=8)
        assert device is None, 'Provisioned device still exposes BLE companion/provisioning'
        print('Provisioned boot does not advertise companion or provisioning', flush=True)
        return
    device = await find(args.name)
    responses = asyncio.Queue()
    buffered = bytearray()
    def received(_, data):
        buffered.extend(data)
        while len(buffered) >= 4:
            if buffered[:2] != b'EZ': raise RuntimeError('Invalid response frame')
            size = int.from_bytes(buffered[2:4], 'little')
            if len(buffered) < size + 4: return
            responses.put_nowait(bytes(buffered[4:4+size]))
            del buffered[:4+size]
    async with BleakClient(device, timeout=20) as client:
        assert client.services.get_service(SERVICE), 'Companion service missing'
        assert client.services.get_service(PROVISIONING), 'Provisioning service missing'
        version = await client.read_gatt_char(VERSION)
        assert 'prov' in json.loads(version), 'Provisioning was not open before claim'
        print('Provisioning open before companion subscription', flush=True)
        await client.start_notify(TX, received)
        for endpoint in ENDPOINTS:
            try: await client.read_gatt_char(endpoint)
            except Exception: pass
            else: raise AssertionError(f'Provisioning read remained open: {endpoint}')
            try: await client.write_gatt_char(endpoint, b'\x00', response=True)
            except Exception: pass
            else: raise AssertionError(f'Provisioning write remained open: {endpoint}')
        print('All five provisioning endpoints reject reads and writes during companion session', flush=True)
        async def exchange(packet):
            frame = b'EZ' + struct.pack('<H', len(packet)) + packet
            # Exercise fragmented framing even with the default ATT MTU.
            for offset in range(0, len(frame), 7): await client.write_gatt_char(RX, frame[offset:offset+7], response=True)
            return await asyncio.wait_for(responses.get(), timeout=10)
        result = await exchange(bytes.fromhex('18012005ba0600'))
        assert b'\xb2\x06' in result, 'No EdgeZ status response'
        result = await exchange(bytes.fromhex('18012005c206020801'))
        assert b'\xc2\x06' in result, 'No device settings response'
        print('Fragmented EdgeZ INIT/status and DeviceSettings GET exchanges passed', flush=True)
        began = time.monotonic()
        while time.monotonic() - began < args.hold_seconds:
            await asyncio.sleep(min(10, args.hold_seconds - (time.monotonic() - began)))
            assert client.is_connected, 'Device slept/disconnected while companion was active'
            if int(time.monotonic() - began) % 30 < 10:
                print(f'Companion still connected at {time.monotonic()-began:.0f}s', flush=True)
        await exchange(bytes.fromhex('18012005ba0600'))
        print(f'Companion responsive after {time.monotonic()-began:.1f}s', flush=True)
    disconnected = time.monotonic()
    device = await find(args.name)
    async with BleakClient(device, timeout=20) as client:
        assert 'prov' in json.loads(await client.read_gatt_char(VERSION)), 'Provisioning not restored'
        print('Provisioning restored on reconnect after companion disconnect', flush=True)
    if args.check_timeout:
        # The reconnect above intentionally starts a fresh 180s window.
        disconnected = time.monotonic()
        for _ in range(6):
            await asyncio.sleep(30)
            print(f'Disconnected grace elapsed {time.monotonic()-disconnected:.0f}s', flush=True)
        await asyncio.sleep(3)
        device = await BleakScanner.find_device_by_filter(lambda d,a: (a.local_name or d.name)==args.name, timeout=8)
        assert device is None, 'BLE still advertising after the three-minute disconnected window'
        print('BLE closed after the three-minute disconnected window', flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--hold-seconds', type=int, default=0)
    parser.add_argument('--check-timeout', action='store_true')
    parser.add_argument('--expect-provisioned', action='store_true')
    asyncio.run(main(parser.parse_args()))

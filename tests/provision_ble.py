#!/usr/bin/env python3
"""Exercise real GATT provisioning using Espressif's reference Security1.

Derives PoP from a private factory image and reads AP credentials from
Git-ignored local config files; never logs either secret. Requires bleak, cryptography, protobuf and an ESP-IDF checkout.
"""
import argparse
import ast
import asyncio
import json
import os
from pathlib import Path
import sys

from bleak import BleakClient, BleakScanner

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", Path.home() / ".platformio/packages/framework-espidf"))
os.environ["IDF_PATH"] = str(IDF)
sys.path.insert(0, str(IDF / "tools/esp_prov"))
from security.security1 import Security1
import proto


def config(path):
    result = {}
    if path.exists():
        for line in path.read_text().splitlines():
            if line.startswith("CONFIG_") and "=" in line:
                name, value = line.split("=", 1)
                if value.startswith('"'):
                    result[name] = ast.literal_eval(value)
    return result


async def main(args):
    local = ROOT / "firmware/zephyr"
    if args.factory_image:
        sys.path.insert(0, str(ROOT / "firmware/scripts"))
        from factory_data import decode, ble_qr
        factory = decode(args.factory_image.read_bytes())
        qr = ble_qr(factory)
        pop = qr["pop"]
        if not args.name: args.name = qr["name"]
    else:
        pop = config(args.pop_config).get("CONFIG_EZ_CAM_PROV_POP", "")
    cfg = {} if args.check_only else config(local / "halowlink2.conf")
    if not args.check_only: cfg.update(config(args.wifi_config))
    ssid = args.ssid or cfg.get("CONFIG_RADAR_HALOW_SSID", "")
    password = cfg.get("CONFIG_RADAR_HALOW_PASSWORD", "")
    if not pop or (not args.check_only and (not ssid or not password)):
        raise RuntimeError("Set local PoP and Wi-Fi config files before provisioning")
    advertisements = await BleakScanner.discover(timeout=20, return_adv=True)
    found = []
    for device, advertisement in advertisements.values():
        # CoreBluetooth can cache the previous firmware's peripheral name.
        name = advertisement.local_name or device.name or ""
        if name == args.name if args.name else name.startswith("PROV_CAM"):
            found.append((device, name))
    if len(found) != 1:
        raise RuntimeError(f"Expected one matching provisioning device, found {len(found)}; use --name if needed")
    device, advertised_name = found[0]
    print("Connecting to", advertised_name, flush=True)
    async with BleakClient(device, timeout=20) as client:
        endpoints = {}
        for service in client.services:
            for characteristic in service.characteristics:
                for descriptor in characteristic.descriptors:
                    if descriptor.uuid.lower() == "00002901-0000-1000-8000-00805f9b34fb":
                        name = bytes(await client.read_gatt_descriptor(descriptor.handle)).decode().rstrip("\0")
                        if name in ["prov-session", "prov-config", "prov-scan", "proto-ver"]:
                            endpoints[name] = characteristic
        if len(endpoints) != 4:
            raise RuntimeError("Expected all four standard endpoint descriptors")
        version = json.loads(bytes(await client.read_gatt_char(endpoints["proto-ver"])))
        if version["prov"]["sec_ver"] != 1:
            raise RuntimeError("Device does not advertise Security 1")
        security = Security1(pop, False)
        request = security.security1_session(None)
        while request is not None:
            await client.write_gatt_char(endpoints["prov-session"], request.encode("latin-1"), response=True)
            response = bytes(await client.read_gatt_char(endpoints["prov-session"]))
            request = security.security1_session(response.decode("latin-1"))
        print("Security 1 mutual proof verified", flush=True)
        if args.check_only:
            return

        async def exchange(endpoint, request, response_type):
            data = security.encrypt_data(request.SerializeToString())
            try:
                await client.write_gatt_char(endpoints[endpoint], data, response=True)
            except Exception as error:
                raise RuntimeError(f"{endpoint} write: {error}") from error
            try:
                data = bytes(await client.read_gatt_char(endpoints[endpoint]))
            except Exception as error:
                raise RuntimeError(f"{endpoint} read: {error}") from error
            result = response_type()
            result.ParseFromString(security.decrypt_data(data))
            return result

        if args.scan:
            pb = proto.wifi_scan_pb2
            request = pb.WiFiScanPayload(); request.cmd_scan_start.blocking = True
            result = await exchange("prov-scan", request, pb.WiFiScanPayload)
            if result.status:
                raise RuntimeError(f"Device scan failed: status {result.status}")
            request = pb.WiFiScanPayload(msg=2); request.cmd_scan_status.SetInParent()
            result = await exchange("prov-scan", request, pb.WiFiScanPayload)
            print("Scan finished:", result.resp_scan_status.scan_finished,
                  "AP count:", result.resp_scan_status.result_count, flush=True)
        pb = proto.wifi_config_pb2
        request = pb.WiFiConfigPayload(msg=2)
        request.cmd_set_config.ssid = ssid.encode()
        request.cmd_set_config.passphrase = password.encode()
        result = await exchange("prov-config", request, pb.WiFiConfigPayload)
        if result.msg != 3 or result.resp_set_config.status:
            raise RuntimeError("Device rejected the network configuration")
        request = pb.WiFiConfigPayload(msg=4); request.cmd_apply_config.SetInParent()
        result = await exchange("prov-config", request, pb.WiFiConfigPayload)
        if result.msg != 5 or result.resp_apply_config.status:
            raise RuntimeError("Device could not apply the network configuration")
        for _ in range(45):
            await asyncio.sleep(1)
            request = pb.WiFiConfigPayload(); request.cmd_get_status.SetInParent()
            result = await exchange("prov-config", request, pb.WiFiConfigPayload)
            state = result.resp_get_status.sta_state
            if state == 0:
                print("Connected; configuration persisted; IPv4:", result.resp_get_status.connected.ip4_addr, flush=True)
                return
            if state == 3:
                raise RuntimeError("Device reported connection/persistence failure")
        raise RuntimeError("Timed out waiting for network connection")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", help="Exact advertised name, when multiple devices are nearby")
    parser.add_argument("--ssid", help="SSID override; password remains in the local file")
    parser.add_argument("--factory-image", type=Path, help="Private factory page used to derive per-device PoP")
    parser.add_argument("--pop-config", type=Path, default=ROOT / "firmware/zephyr/provisioning-secrets.conf")
    parser.add_argument("--wifi-config", type=Path, default=ROOT / "firmware/zephyr/halowlink2-secrets.conf")
    parser.add_argument("--scan", action="store_true")
    parser.add_argument("--check-only", action="store_true", help="Authenticate without changing Wi-Fi credentials")
    try:
        asyncio.run(main(parser.parse_args()))
    except Exception as error:
        parser.exit(1, f"Provisioning test failed: {error}\n")

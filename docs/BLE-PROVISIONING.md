# BLE provisioning and factory data

Build `pio run -e ez-cam01-ble-prov` from `firmware/`. This optional profile
adds provisioning without changing the existing `ez-cam01` baseline. It uses
the Espressif ESP BLE Prov app's standard service, endpoint descriptors and
Security 1 protocol (X25519, SHA256 proof of possession, AES-256-CTR). Security
0 and BLE pairing are not enabled. Tracker-specific MQTT/device-config
endpoints are not part of this profile.

## Configuration precedence

1. Country always comes from the CRC-validated factory record.
2. If factory SSID and password are both present, use them. Saved BLE
   credentials cannot override the factory profile.
3. Otherwise use credentials saved by BLE provisioning in Zephyr Settings.
   nRF54 RRAM uses ZMS as the persistent backend (the equivalent of the ESP32
   NVS role); it does not use erase-dependent NVS.
4. With no credentials, advertise for 120 seconds. BLE stops completely after
   the window expires, or five seconds after a successful status response.
   Normal boots with saved credentials do not start Bluetooth.

The optional EdgeZ driver country hook runs before initial radio startup,
so the firmware does not briefly start in a compiled default country. A
missing, corrupt or unsupported factory country leaves the radio down.
The station scans channels permitted by that country; the legacy factory
channel field is retained for tracker record compatibility. AP DTIM10,
1 MHz channel configuration, 13 ms dynamic PS timeout, camera UART startup
parking, and the 64 KiB JPEG cache are retained. Runtime logs, console and RTT
remain disabled. BLE is only an onboarding activity, not a standby service.

Short presses retain the existing LED toggle and LwM2M button/light updates.
Hold for five seconds to delete the saved BLE network and reboot. Factory
identity, country and factory Wi-Fi are retained. With factory Wi-Fi present,
reset therefore reconnects to that factory profile. Release a button held
across reboot before another reset can be triggered.

## Factory records and flashing

The record is byte-compatible with tracker v2: 211 CRC32-protected bytes in
the dedicated 4096-byte page at `0x165000`. Settings occupy
`0x15c000..0x164fff`; the BLE application's load size stops before Settings.
Application uploads do not intentionally erase either area. Do not use chip
recover or mass erase when preserving these records.

Run these examples from the repository root. Generated private factory pages
and QR payloads belong under Git-ignored `artifacts/`, with owner-only access.
Never commit them or publish them as firmware build artifacts.

```sh
# A new device identity (requires OpenSSL for X25519 key generation).
python3 firmware/scripts/factory_data.py create \
  --serial EZ-CAM01-000001 --country EU --clear-wifi \
  --output artifacts/factory-new.bin

# Update an existing backup, preserving serial, identity keys and page tail.
python3 firmware/scripts/factory_data.py update \
  --input artifacts/factory-before.bin --country EU --clear-wifi \
  --output artifacts/factory-eu.bin

# Public summary only: no password, PoP or private key is printed.
python3 firmware/scripts/factory_data.py inspect artifacts/factory-eu.bin

# Program ONLY the factory page; back up first and verify after programming.
# --replace is required if a valid factory page already exists. The script
# refuses to change an existing serial/public/private identity.
python3 firmware/scripts/factory_data.py flash artifacts/factory-eu.bin \
  --serial 607000398 --replace --backup-dir artifacts/factory-backups

# Official-app QR JSON payload. Treat this as a device credential.
python3 firmware/scripts/factory_data.py qr artifacts/factory-eu.bin \
  --output artifacts/ble-qr.json
```

To embed a factory Wi-Fi profile, use `--ssid` and `--password-file` on create
or update. Keep the password in a private local UTF-8 file, rather than in
shell arguments. WPA3-SAE requires 8–63 password bytes. Omit both Wi-Fi fields
or use `--clear-wifi` to allow BLE onboarding. Files are never silently
overwritten: choose a new output name for each factory revision.

PoP is derived as the first 16 bytes of
`HMAC-SHA256(factory_private_key, "EZ-CAM01 BLE provisioning PoP v1")`, encoded
as 32 lowercase hex characters. The advertised name is `PROV_CAM` plus the
first six lowercase hex characters of SHA256(factory_public_key). Both remain
stable when country or Wi-Fi is updated. The QR JSON contains the standard
`ver`, `name`, `pop`, `transport` fields; encode this JSON into a QR label for
the official app, or enter its PoP manually. Factory private keys are never
sent over BLE. No per-device PoP needs to be compiled into firmware.

## Verification

Host interoperability tests use Espressif's unmodified Python Security 1
client from ESP-IDF and the real C protocol codec. Factory tests compile the
same C record reader and compare it with the host writer.

```sh
python3 tests/test_esp_prov.py
python3 tests/test_factory_data.py
# Optional real device test, using approved local AP credentials.
python3 -m venv /tmp/ezcam-ble
/tmp/ezcam-ble/bin/pip install -r tests/requirements.txt
/tmp/ezcam-ble/bin/python tests/provision_ble.py \
  --factory-image artifacts/factory-eu.bin --scan
```

The live helper reads the AP password from ignored
`firmware/zephyr/halowlink2-secrets.conf`; it never prints it or the PoP.
`--check-only` authenticates without reading or applying AP credentials.
See [validation results](VALIDATION.md) for hardware results and limitations.

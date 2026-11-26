# BLE provisioning and factory data

Build `pio run -e ez-dev54` from `firmware/`. The unified firmware includes
BLE provisioning. It uses
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
4. With no credentials, advertise for 180 seconds. BLE stops completely after
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

When camera mode is selected, UART22 driver initialization is deferred until the first
Capture execute. Boot only parks TX/RX and turns camera power off; it does
not initialize the UART just to suspend it. After capture, the same normal
IRQ-disable / suspend / sleep-pinctrl shutdown path is used. This fixes the
reported higher sleep floor before the first capture. The operator confirmed
3 mW after a cold start with J-Link disconnected, before any Capture or
Observe subscription; see VALIDATION.md.

Short presses retain the existing LED toggle and LwM2M button/light updates.
Hold for five seconds to delete the saved BLE network and reboot. Factory
identity, country and factory Wi-Fi are retained. With factory Wi-Fi present,
reset therefore reconnects to that factory profile. Release a button held
across reboot before another reset can be triggered.

## Transport standby and two-stage provisioning wake

An unfinished 180-second provisioning window disables Bluetooth and enters
nRF54 System OFF when `CONFIG_EZ_DEV54_PROV_MOTION_SLEEP=y`. HaLow and external
UART-device power are switched off, signals are parked and the indicator is off.
Normal saved/factory-provisioned operation and connected WNM are unchanged.

The kernel system timer is disabled immediately before System OFF so pending
kernel deadlines cannot periodically wake standby. Boot preserves and checks
the IMU's latched 6D event before clearing its registers; a different System OFF
wake cause cannot bypass the first stage.

There are two successive checks, with no direct interrupt-to-BLE path:

1. During System OFF only the accelerometer runs, at 26 Hz low power. Its
   latched 6D orientation interrupt on INT1/P0.02 wakes the MCU. The default
   hardware classification angle is 60 degrees (factory choices 50/60/70/80).
   This recognizes six gravity-based orientations, not every arbitrary rotation.
2. After this interrupt the accelerometer stops and the gyroscope runs at
   26 Hz, +/-250 degrees/second, for at most three seconds. The first 200 ms
   are ignored for sensor settling. At least two axes must **simultaneously
   exceed 30 degrees/second in absolute value for 100 ms** before BLE opens.
   A missing sample interval over 100 ms breaks confirmation. There is no
   0.5 g acceleration-difference condition or accumulated-angle condition.

Success opens a new 180-second provisioning window. Timeout, a single-axis
rotation, a short spike, or sensor failure cannot open BLE; the gyroscope is
powered down on every exit and the accelerometer orientation interrupt is
rearmed before returning to System OFF. An IMU preparation error keeps BLE off
and retries after 30 seconds rather than sleeping without a working wake source.
The checks filter ordinary bumps and single-axis movements, but real transport
handling can still satisfy both. Measure false-trigger frequency with actual
packaging; this is not a guarantee of zero shipping wake-ups.

The CRC-protected `EZPM` version-3 extension at factory-page offset 272 stores
`gyro_threshold_dps` (5–180 degrees/second) and `pose_interrupt_deg` (50/60/70/80).
Each wake reads the factory configuration anew; absent, invalid or older motion
extensions use 30 degrees/second and 60 degrees. The 211-byte identity record,
PoP and hardware extension remain unchanged. No threshold is written to NVS.

The gyro check needs no pre-sleep gravity vector. No application SRAM is retained
in System OFF, and the full 256 KiB application SRAM is available. Runtime logs,
console and RTT remain disabled. Actual standby current and wake reliability
still require board measurements with the debugger disconnected.

Example (creates a private image; programming is a separate operation):

```bash
python3 firmware/scripts/factory_data.py update \
  --input /private/path/factory-before.bin \
  --gyro-threshold-dps 30 --pose-interrupt-deg 60 \
  --output /private/path/factory-updated.bin
```

`--clear-motion-threshold` restores both defaults. `prov_power_error` remains
available as a passive SWD diagnostic.

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
  --serial 5037CDF35BB6 --model EZ-DEV54-GPS --country EU --clear-wifi \
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
as 32 lowercase hex characters. New factory records use exactly 12 uppercase hex characters for `serial`,
for example `5037CDF35BB6`. The advertised name is `EZ_` plus that serial,
and the LwM2M endpoint is the serial itself. Neither is derived separately
from a public-key hash or radio MAC. The QR JSON contains the standard
`ver`, `name`, `pop`, `transport` fields; encode this JSON into a QR label for
the official app, or enter its PoP manually. Factory private keys are never
sent over BLE. No per-device PoP needs to be compiled into firmware.

The original KDF label and on-flash `ezcam/wifi` settings key are retained.
The PoP derivation is unchanged. There is no serial migration, alias mapping,
or automatic rewrite of existing records. `update` preserves the stored serial unless `--serial` is explicitly supplied.
Changing a live device serial additionally requires `flash --previous-serial`
with the exact old value. Identity keys cannot be replaced by this update path.
New identity creation is separate from applying firmware to an existing device.

Factory model is stored in an optional 48-byte CRC-protected `EZMD` v1 extension
at page offset 288, following the existing hardware and motion extensions. It
contains up to 32 printable ASCII characters. New records default to `EZ-DEV54-GPS` for collars; use `--model EZ-DEV54-CAM`
for cameras. The app classifies the device using this model resource. Explicit model updates do not
change serials or keys. Missing or invalid model extensions use `EZ-DEV54`.
LwM2M Device resources `/3/0/1` and `/3/0/2` report factory model and serial.


## Verification

Host interoperability tests use Espressif's unmodified Python Security 1
client from ESP-IDF and the real C protocol codec. Factory tests compile the
same C record reader and compare it with the host writer.

```sh
python3 tests/test_esp_prov.py
python3 tests/test_factory_data.py
# Optional real device test, using approved local AP credentials.
python3 -m venv /tmp/ezdev54-ble
/tmp/ezdev54-ble/bin/pip install -r tests/requirements.txt
/tmp/ezdev54-ble/bin/python tests/provision_ble.py \
  --factory-image artifacts/factory-eu.bin --scan
```

The live helper reads the AP password from ignored
`firmware/zephyr/halowlink2-secrets.conf`; it never prints it or the PoP.
`--check-only` authenticates without reading or applying AP credentials.
See [validation results](VALIDATION.md) for hardware results and limitations.

External hardware factory flags and persistent overrides are documented in [hardware selection](VARIANTS.md). The default without either is base.

### Provisioned display name

Firmware advertises `mqtt_config`. After Security 1 authentication the app
sends the existing provisioning JSON payload, including `deviceName`
(1–128 UTF-8 bytes, no NUL/control characters), through the shared encrypted
`mqtt-config` credential endpoint before Wi-Fi configuration. Its JSON response
confirms `ok` and `persisted`; failure stops provisioning. The name is stored
separately as `ezcam/name`, preserving the existing Wi-Fi record and factory
identity. Read the name through vendor resource `/31029/0/7`. Device model and
serial remain `/3/0/1` and `/3/0/2`; the endpoint remains the factory serial.
Older firmware without this capability keeps its existing provisioning path.

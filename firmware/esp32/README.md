# EZ-DEV01 / HT-HC33 firmware

Standalone ESP-IDF firmware for the HT-HC33 (ESP32-S3 + MM6108). The parent
repository also contains the existing EZ-DEV54 nRF54L15 project at `firmware/`;
its application source has not been moved here.

## Build and flash

```sh
git submodule update --init --recursive
python3 -m pip install platformio==6.1.19
pio run -d firmware/esp32 -e ez-dev01
pio run -d firmware/esp32 -e ez-dev01 -t upload --upload-port /dev/ttyUSB0
pio device monitor --port /dev/ttyUSB0 --baud 115200
```

The default `ez-dev01` image uses BLE Security 1 and Wi-Fi HaLow station
provisioning. The legacy `heltec-hc33`, `ht-hc33-sensor`,
`ht-hc33-halow-wake-test` and `heltec-hc33-mesh-ps` targets remain available
for their existing mesh, zero-touch and RF test use cases. They do not select
the new onboarding behavior.

## Onboarding and companion

- Factory identity is required in the existing `factory_data` partition at
  `0x7e3000`; normal uploads preserve it and NVS. Never use erase-all to update.
- The BLE name is `EZ_{serial}`. Security 1 uses the same factory-key-derived
  proof of possession and packet codec as EZ-DEV54.
- Before provisioning, BLE is available for three minutes. Any live connection
  keeps the MCU awake. Subscribing to the companion TX characteristic claims
  companion mode and closes reads/writes on all provisioning endpoints.
- Disconnect clears session/staged data, restores provisioning and starts a
  fresh three-minute window. Expiry enters deep sleep; USER/GPIO0 wakes it.
- Companion supports status and read-only device settings. It does not forward
  mesh chat packets. This is the same current companion scope as EZ-DEV54.
- HaLow network scanning is available through `prov-scan` (active scan).
- Standard `prov-config` supplies SSID/password; the existing `mqtt-config`
  endpoint saves `deviceName`. No extra endpoint is introduced.
- Credentials are persisted only after HaLow association and DHCP succeed.
  Configured devices do not start provisioning or companion BLE.
- Hold USER for five seconds to clear this image's saved Wi-Fi and restart.
  Factory identity, keys and unrelated settings remain intact. Factory Wi-Fi,
  if present, retains precedence and is never removed by this action.

## Local LwM2M

After DHCP, the client uses the current HaLow gateway at UDP port 5683.
If a local-only AP omits a router, it uses the DHCP server identifier when
that server is on the same subnet. No address is compiled into the image.
The endpoint and `/3/0/2` are the factory serial; `/3/0/1` is the factory model
extension (or `EZ-DEV01` for older factory records). `/31029/0/7` returns the
saved device name, falling back to serial. Device Execute `/3/0/4` reboots;
`/3/0/5` clears only this image's saved Wi-Fi then reboots.

No settings or factory-data migration runs. Existing records and keys are read
in place. For new factory data, use the parent's `firmware/scripts/factory_data.py`
with a 12-character uppercase hex serial and `--model EZ-DEV01`; its output is a
compatible factory page. Its J-Link flash command is nRF-specific, so write the
HT-HC33 page using esptool at `0x7e3000` instead. The imported legacy factory
helper in `scripts/` remains available for existing manufacturing workflows.

## Dependencies and licenses

The ESP platform is pinned to the source revision recorded in [SOURCE.md](SOURCE.md).
The build script applies the owned station-adapter patch to that pinned checkout;
`main/dev01.cmake` selects the factory regulatory domain before RF initialization
and retains the HC33 reset-safe GPIO implementation.

See the repository's [firmware licensing notice](../../FIRMWARE-LICENSING.md).
Vendor blobs, RF libraries, Wakaama and their notices retain their licenses.

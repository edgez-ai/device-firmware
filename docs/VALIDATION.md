# Migration validation

Date: 2026-11-24.

The standalone default `ez-cam01` build passed in 164.65 seconds using
EZ-CAM01's own submodules. Inherited `MORSE_ZEPHYR_MODULE` and `WAKAAMA_CORE`
environment overrides were removed for this build. No sibling repository
source paths occur in the generated compilation database; Wakaama resolves
to this repository's `third_party/wakaama`. All dependency clones contain
independent Git objects with no alternates pointing into the source repo.

- Flash: 845,452 bytes; static RAM: 247,988 bytes.
- HEX SHA-256: `f1e00943e95e984352665b3e7bffb7afc02135573de791175242ae2d0e92172a`.
- This is the same HEX hash recorded for the original validated 3 mW camera
  image. There is no application-binary change from this migration.
- All imported application C/header files match source commit
  `cb9130aa56edd114ebb9330041d12c154c9c5b24` byte for byte.
- Effective camera CMake feature arguments and linker flags match the original
  `edgez-nrf54l15-fgh200m-halow-camera-halowlink2-edgez` environment.
- The build pins EdgeZ `mm-iot-zephyr` at `947f1e4`, with nested SDK/radio
  revisions retained, and EdgeZ Wakaama client core at `b686079`.

Configuration checks confirmed EU, 13 ms dynamic PS timeout, listen interval
0 (follow DTIM), 600-second health check, UDP server discovery through DHCP,
interrupt-driven UART, MQTT disabled, and logs/console/SEGGER RTT disabled.
The optional SPI IRQ pull-up remains disabled. No SPI input/high-output or
WNM experiment is enabled.

The new J-Link uploader was checked with a fake executable: resolved image
paths containing spaces, target/probe selection, reset/load/reset/run command
ordering, and invalid-speed rejection passed. This did not access hardware.
The uploader help command also passed. The import's Git diff checks passed;
local password files and artifacts are ignored and are not staged.

The original carrier's approximately 3 mW pre-first-photo cold-start result
is inherited validation, not a new measurement. No image was flashed and no
Capture execute was performed during the initial migration. The carrier
remained in its separate radar power-benchmark configuration until the
subsequent deployment below.

HEX, ELF, build log and build metadata are saved locally in the Git-ignored
`artifacts/ez-cam01-baseline/` directory. These binaries include the local
runtime configuration and are not committed. The source import and validation notes are included in the camera migration
commit; local credentials and build artifacts remain excluded.

## Build and flash from EZ-CAM01 — 2026-11-24

At the operator's request, ran `pio run -t upload` from `firmware/`, using
only this repository's module/core paths and its J-Link upload hook. Build
and upload passed in 140.80 seconds on J-Link serial 607000398, device
NRF54L15_M33, SWD 4 MHz. J-Link reported programming and verification passed,
then reset/run. The HEX still matches the original validated camera digest:
`f1e00943e95e984352665b3e7bffb7afc02135573de791175242ae2d0e92172a`.
The radar power-only fixture has now been replaced by the camera firmware.

The gateway API initially returned no clients. After flashing, the device
registered as `halow-5037cdf35bb6` with lifetime 300 seconds. All three live
LwM2M resource reads returned HTTP 200 / CoAP 2.05:

| Path | Result |
| --- | --- |
| /5001/0/2 | Image length zero (REST adapter displays BOOLEAN false) |
| /5001/0/3 | 640x480 |
| /5001/0/5 | 65536-byte buffer |

Direct SWD memory reads, without an additional reset/halt command, confirmed
camera `ready=1` at 0x20015308 and `jpeg_length=0` at 0x2000237c. UART22,
SPIM00 and SAADC ENABLE were all zero; UART P1.11/P1.12 PIN_CNF were 2/2;
P1.09 camera power was output low. GPIO1 OUT=0x1090 and DIR=0x82b0.
GPIO2 OUT=0x21 confirmed HaLow power on and the active-low LED off. These
are the normal camera settings, rather than the no-HaLow radar fixture.

Generated configuration retains logs/console/SEGGER RTT disabled, SPI IRQ
pull-up disabled, 13 ms dynamic PS timeout, listen interval 0 and 600-second
health check. No AP configuration was changed. No Capture execute or LED
write was sent, so this verifies startup before first photo. Queries stopped
after verification. Disconnect SWD and cold-start without taking a photo
before measuring the flat sleep floor; this run contains no new meter result.

Build/upload, startup-register and LwM2M logs are saved under the local
Git-ignored `artifacts/ez-cam01-baseline/` directory. These source files and validation notes are included in the camera migration
commit at the operator's request. No push is part of that request.

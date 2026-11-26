# EZ-DEV54

nRF54L15 + FGH200M/MM8108 Wi-Fi HaLow development firmware with base,
VC0706 camera, IWRL6432 radar and L76K GPS runtime modes.

The initial implementation comes from the validated `openwrt-firmware`
camera baseline `cb9130a`, with the EdgeZ Morse driver fork retained.
It powers the camera only for Capture, caches the JPEG locally in a 64 KiB
buffer, cuts camera power afterward, and exposes it through LwM2M/CoAP UDP.
Startup parks UART immediately, including the already-suspended PM case;
the rejected extra UART resume/suspend cycle is not included.

## Build

One firmware supports **base**, **VC0706 camera**, **IWRL6432 radar** and
**L76K GPS**. Hardware is selected at boot: saved LwM2M override, then factory
flag, then base. Configure `/31029/0/0` with 0–3; changes persist in flash and
activate after reboot. See [hardware selection, wiring and resources](docs/VARIANTS.md).

This project was formerly EZ-CAM01. The build environment is `ez-dev54` and
feature flags use `EZ_DEV54_`. Factory PoPs and saved Wi-Fi remain compatible.

Install PlatformIO and initialize the pinned Git submodules:

```sh
git submodule update --init --recursive
cd firmware
cp zephyr/halowlink2-secrets.conf.example zephyr/halowlink2-secrets.conf
# Edit halowlink2-secrets.conf with the AP's WPA3-SAE password.
pio run
```

The default environment is `ez-dev54`, targeting the existing EdgeZ carrier
through the XIAO nRF54L15 CPU definition. Gateway SSID defaults to
`halowlink2`; EU region and DHCP-based server discovery are retained.
No fixed Jetson IP is used. The LwM2M server must listen on UDP 5683 on the
HaLow gateway. Configure the AP for 1 MHz and DTIM10; beacon/DTIM settings
are AP settings, not programmed by this firmware.

`zephyr/secrets.conf` is optional. Local credentials and build artifacts are
Git-ignored. The copied local credentials, when present, must stay untracked.

Dependencies are local submodules, with no sibling-repository lookup:

| Dependency | Pinned revision |
| --- | --- |
| EdgeZ `mm-iot-zephyr` | `7878f9dfb71875f247d76eedba9d683c43ce79a8` |
| EdgeZ Wakaama client core | `04e18bbcc561d820bd4000014fb60cd4ed9acbed` |
| Seeed PlatformIO platform | `c41f41ec51c8cf91550adc8b1487e1b093c8605c` |
| Zephyr / Arm toolchain | 4.4.0 / 14.2.1, as pinned in `firmware/platformio.ini` |

Nested SDK/radio dependencies are pinned by the EdgeZ module. Update a pin
deliberately; `git submodule update --remote` is not part of a normal build.

## BLE provisioning

The unified environment includes compatibility with the
Espressif ESP BLE Prov app, persistent onboarding and a five-second button
reset. Country comes from factory storage; a complete factory Wi-Fi profile
has priority over BLE-saved credentials. See the [factory and BLE guide](docs/BLE-PROVISIONING.md)
for the provisioning/flashing script and verification commands.

The factory-country hook is committed on the module’s `edgez` branch.
The submodule remains pinned to the exact revision above.

## Flash over J-Link

```sh
cd firmware
pio run -t upload
```

Install SEGGER J-Link first. If needed, set `JLINK_EXE` and `JLINK_SERIAL`.
The uploader uses `NRF54L15_M33`, SWD at 4 MHz, image programming/verification,
then reset/run. It does not automatically recover or mass-erase the chip.
No firmware is flashed merely by running `pio run`.

## LwM2M resources

The endpoint is the factory serial; new factory records use 12 uppercase hex characters.
BLE advertises `EZ_{serial}`. Device `/3/0/1` reports the factory model and
`/3/0/2` reports that same serial. Existing factory data is never migrated. The existing Camera object is
`5001/0`; button and light behavior are retained.

| Path | Operation | Meaning |
| --- | --- | --- |
| `/5001/0/0` | Execute | Power camera, capture, cache JPEG, power camera off |
| `/5001/0/1` | Read | Cached JPEG via CoAP Block2 |
| `/5001/0/2` | Read | Cached image length; zero before capture |
| `/5001/0/3` | Read/Write | Resolution string, default `640x480` |
| `/5001/0/5` | Read | JPEG buffer capacity, 65536 bytes |
| `/3311/0/5850` | Read/Write/Observe | LED/light state |
| `/3347/0/5500` | Read/Observe | Physical button state |
| `/3313/0/5702..5704` | Read/Observe | IMU acceleration X/Y/Z in m/s2 |
| `/3334/0/5702..5704` | Read/Observe | IMU angular rate X/Y/Z in rad/s |
| `/3/0/7` | Read | Power source voltage, instance 0, in mV |
| `/3/0/9` | Read | Battery percentage (3.0–4.2 V estimate; above 5 V reports 100%) |

[Sensor lifecycle and wiring](docs/SENSORS.md): ordinary reads enable and
sample only on demand. IMU Observe keeps the requested engine running and
reports changes through native LwM2M; cancelling the last observer returns it
to chip power-down. The ADC divider is off between voltage reads.

Resolution strings: `160x120`, `320x240`, `640x480`, `800x600`,
`1024x720`, `1024x768`. The intermediate 800x600 and 1024x720 modes use
the existing crop path from the camera's 1024x768 mode. Not every camera
module supports the extended modes. JPEGs must fit the 64 KiB cache.
An Execute response reports completion; the image is read from resource 1.

## Carrier wiring and standby

| Signal | nRF54L15 pin |
| --- | --- |
| Camera power EN | P1.09 |
| Camera UART TX / RX | P1.12 / P1.11 |
| Button / active-low LED | P1.08 / P2.05 |
| HaLow power EN | P2.00 |
| HaLow CS / SCK / MOSI / MISO | P1.07 / P2.01 / P2.02 / P2.04 |
| HaLow IRQ / BUSY / WAKE / RESETn | P1.06 / P1.10 / P1.05 / P1.04 |

The original mixed button-SENSE/BUSY-GPIOTE behavior is preserved. RTT,
logging and the console are disabled in the default image. The fork's
`SPI_IRQ_PULL_UP` support remains available; its pull-up is disabled in the
validated profile. The camera stays off and UART suspended before the first
photo. No SPI input-gating/high-output experiment is imported.

[Timed WNM deep sleep](docs/SLEEP.md) is available through LwM2M object
`31026`. It runs only after Execute; normal standby continues to use DTIM10.
The [connection watchdog](docs/CONNECTION-WATCHDOG.md) checks native LwM2M ACKs
during normal operation and after wake, with staged reconnect recovery.
The optional `CONFIG_EZ_DEV54_WNM_PARK_PINS=y` setting adds WNM-only signal-pin
parking for a power comparison; see the [pin-state table](docs/SLEEP.md#experimental-wnm-only-gpio-parking).

The original carrier measured about 3 mW at the flat sleep floor after a
cold boot with SWD disconnected, before any photo. That is inherited source
validation, not a new current measurement for this repository and not the
average including DTIM wakes, captures or transfers. Disconnect SWD and
cold-start before comparing measurements; do not take a photo first.

For optional cached sleep diagnostics, pass `-DRADAR_HALOW_SLEEP_DIAG=ON` to CMake.
This exposes the inherited diagnostic commands through `/3347/0/5750`.
Use the default image for the validated baseline comparison.

Some internal files and Kconfig symbols retain the legacy `RADAR_` naming
to preserve the working implementation. Radar acquisition and MQTT are not
started by the camera profile. See [migration notes](docs/MIGRATION.md).

## License notices

**EdgeZ HaLow Mesh (`edgez-halow-mesh`) is licensed for non-commercial use
and internal evaluation under the [EdgeZ Non-Commercial and Commercial
Software License, Version 1.0](licenses/EDGEZERON-NON-COMMERCIAL.txt).
Commercial use requires a separate written license from EdgeZeron AI AB.**

**Morse Micro MM-IoT-SDK precompiled libraries and binaries covered by the
[Morse Micro Binary Distribution License Agreement](licenses/LicenseRef-MorseMicroBDL.txt)
remain subject to that agreement.** SDK source components have separate
licenses; the binary license does not cover the entire SDK.

See [third-party notices](THIRD-PARTY-NOTICES.md) for component scope and
upstream references. This repository's [Apache-2.0 license](LICENSE) does not
replace dependency-specific terms.

# EZ-CAM01

nRF54L15 + FGH200M/MM8108 Wi-Fi HaLow camera firmware for the VC0706 UART
camera. This repository is the home for further camera and power work.

The initial implementation comes from the validated `openwrt-firmware`
camera baseline `cb9130a`, with the EdgeZ Morse driver fork retained.
It powers the camera only for Capture, caches the JPEG locally in a 64 KiB
buffer, cuts camera power afterward, and exposes it through LwM2M/CoAP UDP.
Startup parks UART immediately, including the already-suspended PM case;
the rejected extra UART resume/suspend cycle is not included.

## Build

Install PlatformIO and initialize the pinned Git submodules:

```sh
git submodule update --init --recursive
cd firmware
cp zephyr/halowlink2-secrets.conf.example zephyr/halowlink2-secrets.conf
# Edit halowlink2-secrets.conf with the AP's WPA3-SAE password.
pio run
```

The default environment is `ez-cam01`, targeting the existing EdgeZ carrier
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
| EdgeZ `mm-iot-zephyr` | `31e94bda2b18709b4d5d088f8462670bd079da0e` |
| EdgeZ Wakaama client core | `b68607938ce43160cd9abe1bd14bcc3d483e9c04` |
| Seeed PlatformIO platform | `c41f41ec51c8cf91550adc8b1487e1b093c8605c` |
| Zephyr / Arm toolchain | 4.4.0 / 14.2.1, as pinned in `firmware/platformio.ini` |

Nested SDK/radio dependencies are pinned by the EdgeZ module. Update a pin
deliberately; `git submodule update --remote` is not part of a normal build.

## BLE provisioning

An optional `ez-cam01-ble-prov` environment adds compatibility with the
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

The endpoint is derived from the HaLow MAC. The existing Camera object is
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
photo. No SPI input-gating/high-output or WNM experiment is imported.

The original carrier measured about 3 mW at the flat sleep floor after a
cold boot with SWD disconnected, before any photo. That is inherited source
validation, not a new current measurement for this repository and not the
average including DTIM wakes, captures or transfers. Disconnect SWD and
cold-start before comparing measurements; do not take a photo first.

For optional cached sleep diagnostics, build `pio run -e ez-cam01-idle-diag`.
This exposes the inherited diagnostic commands through `/3347/0/5750`.
Use the default image for the validated baseline comparison.

Some internal files and Kconfig symbols retain the legacy `RADAR_` naming
to preserve the working implementation. Radar acquisition and MQTT are not
started by the camera profile. See [migration notes](docs/MIGRATION.md).

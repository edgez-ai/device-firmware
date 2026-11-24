# UART device variants

The four build profiles target the same nRF54L15 + FGH200M carrier. Select
exactly one UART peripheral at build time. All four include ESP-compatible BLE
provisioning, factory identity/country, saved credentials, UDP LwM2M over HaLow,
Device/IMU resources, LED/button, the connection watchdog and timed radio WNM.
RTT, console and logging remain disabled. WNM signal-pin parking remains an
independent, default-off experiment.

| Variant | PlatformIO environment | UART22 | Peripheral power P1.09 |
| --- | --- | --- | --- |
| Base | `ez-cam01-base` | Disabled; TX/RX disconnected | LOW |
| Camera | `ez-cam01-camera` | VC0706, 921600 baud; initialized on Capture | Off between captures |
| Radar | `ez-cam01-radar` | IWRL6432, TI baud correction enabled | On during normal operation |
| GPS | `ez-cam01-gps` | L76K, 9600 baud; initialized on Acquire | Off after fix, timeout or cancellation |

Build from `firmware/`, for example:

```sh
pio run -e ez-cam01-base
pio run -e ez-cam01-camera
pio run -e ez-cam01-radar
pio run -e ez-cam01-gps
```

Add `-t upload` to build and flash the selected profile with the existing
J-Link uploader. Building alone does not access hardware. Use one profile at a
time on the shared connector. Firmware updates preserve the factory and Settings
partitions. They do not automatically reopen BLE if valid credentials are saved;
hold the button for five seconds to clear BLE-saved credentials and reprovision.
The device-specific PoP does not change between variants.

Legacy `ez-cam01`, `ez-cam01-ble-prov`, `ez-cam01-idle-diag` and
`ez-cam01-wnm-pins` remain camera profiles. The default environment remains
`ez-cam01`. The new four variants all include BLE provisioning, including the
new `ez-cam01-camera` equivalent of the BLE camera profile.

## Wiring and radar behavior

The carrier's external peripheral enable is active HIGH on P1.09. The camera
uses MCU TX P1.12 / RX P1.11. Radar and GPS use MCU TX P1.11 / RX P1.12;
connect peripheral TX to MCU RX and peripheral RX to MCU TX. GPS uses the
existing tracker carrier pinout. Confirm module EN/WAKE/reset strapping on the
attached L76K board; no additional GNSS wake/reset pins are assumed by this
four-wire UART/supply profile. Pin selections are in the variant devicetree
overlays under `firmware/zephyr/boards/`.

The radar variant retains the existing receive-only IWRL6432 supervisor and
TLV 1101 decoder. It expects radar firmware that autostarts and emits the
supported human-detection reports. This profile does not flash the radar MCU
or send a CLI configuration. Native presence `/3302/0/5500` and distance
`/3330/0/5700` are registered only in the radar variant. Button presses control
the local LED; the base and GPS variants publish no simulated radar results.

Timed WNM is a **radio** sleep. The radar continues operating during WNM, and
the latest radar state is published when radio processing resumes. Therefore
the camera/GPS/base sleep power floors are not promises for the powered radar
variant. No physical power or radar-hardware validation is implied by a build.

## On-demand L76K GPS

Execute `/31028/0/0` to request one fix. Execute returns immediately, and
acquisition runs in the existing Wakaama event loop using UART interrupts.
It adds no periodic GPS timer or worker thread while idle. A valid
checksum-verified active GPRMC/GNRMC sentence supplies latitude, longitude and
its UTC measurement timestamp. Zero latitude/longitude are valid coordinates.
No location is fabricated before a valid fix.

| Path | Access | Meaning |
| --- | --- | --- |
| `/31028/0/0` | Execute | Acquire one fix |
| `/31028/0/1` | Read/Write | Acquisition timeout, 10–600 seconds; default 120 |
| `/31028/0/2` | Read/Observe | State: 0 idle, 1 acquiring, 2 fixed, 3 timed out, 4 error, 5 cancelled |
| `/31028/0/3` | Read/Observe | Last result: 0 success, negative platform errno on failure |
| `/31028/0/4` | Read | Cached fix age in seconds, or -1 before the first fix |
| `/31028/0/5` | Read/Observe | Successful acquisition count since boot |
| `/31028/0/6` | Read/Observe | Whether a cached valid fix exists |
| `/6/0/0` | Read/Observe | Cached latitude, decimal degrees |
| `/6/0/1` | Read/Observe | Cached longitude, decimal degrees |
| `/6/0/5` | Read/Observe | UTC Unix timestamp from the RMC date/time |

The receiver supply is cut and UART pins parked after the first valid fix,
timeout or hardware error. WNM requests and LwM2M context loss cancel an ongoing
acquisition and also power it off. WNM does not automatically restart GPS on
wake. A previous good fix remains readable after a later failure; check its age
and acquisition state. Location reads return CoAP 5.03 before the first fix.
Reading or observing the cached location never starts the receiver. Fixes are
RAM-only and cleared on boot. Altitude, accuracy and speed are not reported.

Location object 6 is standard LwM2M. Object 31028 is an EdgeZ vendor object,
not an OMA-registered identifier. Its UI model is supplied in
[`lwm2m/31028.xml`](lwm2m/31028.xml); install it in the server's model directory
when using a generic LwM2M UI. This firmware change does not deploy server models.

Protocol references: [OMA Location object](https://www.openmobilealliance.org/RELEASE/LightweightM2M/V1_2_2-20240613-A/HTML-Version/OMA-TS-LightweightM2M_Core-V1_2_2-20240613-A.html),
[Quectel L76K board guide and protocol documents](https://wiki.seeedstudio.com/get_start_l76k_gnss/).

## Validation

`python3 tests/test_gps.py` runs the actual NMEA parser and GPS object under
ASan/UBSan, with fake UART/power/clock boundaries. It covers valid and invalid
checksums, coordinate/date bounds, cached reads, deferred acquisition, timeout,
startup/read failures and WNM/context cancellation. Real GPS acquisition and
peripheral power still require the corresponding hardware and meter tests.
The GPS UART boundary is also compiled under sanitizers with fake driver APIs
to check deferred initialization, ISR handoff, power sequencing and cleanup
after UART initialization/callback/resume failures.

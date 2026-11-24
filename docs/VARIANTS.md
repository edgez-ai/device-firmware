# Runtime external hardware selection

One `ez-dev54` firmware includes all four hardware modes. BLE onboarding,
HaLow, Device/IMU, LED/button, watchdog and WNM services are common. RTT,
console and logging stay disabled. No peripheral is automatically probed.

Build with `cd firmware && pio run -e ez-dev54`. Add `-t upload` to flash.
The former per-variant build environments have been replaced by this image.

Boot selection priority is **saved override > valid factory flag > base**.
An absent or invalid flag defaults safely to base. The saved override uses
Zephyr Settings/ZMS in nonvolatile flash (the nRF equivalent of NVS).
Existing devices with no hardware flag or override boot as base.

| Value | Hardware | UART22 MCU TX / RX | Power P1.09 |
| --- | --- | --- | --- |
| 0 | Base | Both disconnected; UART never initialized | Off |
| 1 | VC0706 camera | P1.12 / P1.11, 921600 baud | On only during capture |
| 2 | IWRL6432 radar | P1.11 / P1.12, 115200 with TI baud correction | On during operation |
| 3 | L76K GPS | P1.11 / P1.12, 9600 baud | On request, then off |

The connector is shared; only one external device is supported at a time.
The radar expects separately flashed autostart firmware emitting supported
human-detection TLV reports. The host does not flash or configure the radar MCU.
Radar presence `/3302/0/5500` and distance `/3330/0/5700`, camera resources,
and GPS objects are registered only for the selected mode. Object 31029 is
always available. UART pin routing and baud are chosen before deferred init.

## LwM2M configuration

| Path | Access | Meaning |
| --- | --- | --- |
| `/31029/0/0` | RW | Desired hardware: 0 base, 1 camera, 2 radar, 3 GPS |
| `/31029/0/1` | R | Hardware active this boot |
| `/31029/0/2` | R | Factory hardware, or -1 if absent/invalid |
| `/31029/0/3` | R | Desired selection source: 0 default, 1 factory, 2 saved override |
| `/31029/0/4` | R | Reboot pending because desired differs from active |
| `/31029/0/5` | E | Delete saved override and restore factory/default selection |
| `/31029/0/6` | R | Last configuration error; 0 means success |

Writing resource 0 saves the selection before returning success. A change
of active type schedules a cold reboot after two seconds so the response can
leave first. Writing the active type saves an override without rebooting.
Execute resource 5 also reboots when the resulting selection differs.
Factory identity, country and saved Wi-Fi credentials are preserved.
Object 31029 is an EdgeZ vendor identifier. Import
[`lwm2m/31029.xml`](lwm2m/31029.xml) into a generic server UI as needed.

## Factory flag

Use `firmware/scripts/factory_data.py create --hardware camera ...` or
`update --input existing.bin --hardware gps --output updated.bin`.
`--clear-hardware` removes the flag. Updates retain the 211-byte v2 identity
layout, private keys, PoP derivation and unrelated factory page contents.
The extension occupies bytes 256–271, uses magic `EZHW`, version 1, length
16, a one-byte hardware value, three reserved zero bytes and CRC32 of the
first 12 bytes. Unknown occupied extension bytes are refused on update.
Factory images and backups contain private keys and must remain outside Git.
Programming the factory page uses the existing explicit factory flash command
with backup/verification; ordinary firmware upload preserves this page.

Timed WNM sleeps the radio. A selected radar continues operating during WNM;
its power consumption remains part of the device power budget. Base, camera
and GPS leave their external power rail off while idle. Physical peripheral
operation and power measurements require testing on the corresponding hardware.

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

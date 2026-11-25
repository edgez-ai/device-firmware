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

## Factory country and ESP-compatible BLE provisioning — 2026-11-24

Built and flashed the optional `ez-cam01-ble-prov` profile. Clean build passed
in 185.76 seconds; a final incremental consistency build passed in 14.20
seconds. Flash usage is 960584 bytes and static RAM usage is 249309/262144
bytes (95.1%). The 64 KiB JPEG cache and existing 64 KiB kernel heap remain.
Firmware HEX SHA256:
`b493b12473b31a8f0f1b21d470875d95b40646b9ff3c4c9f1e67d5e3a753427a`.
J-Link programmed and verified 970752 bytes, then reset/run. No mass erase
or chip recovery was performed.

The pre-existing tracker v2 factory page was backed up locally. It had US
country and empty Wi-Fi fields. With explicit operator authorization, changed
country to EU and retained empty Wi-Fi, preserving serial, public/private
identity keys, channel field and trailing page bytes. The dedicated page was
programmed and verified at `0x165000`. The factory-derived advertised name is
`PROV_CAM95a863`. The EdgeZ module's optional initial-country hook reads the
validated factory record before RF startup; this feature is committed on module
branch `edgez` at `31e94bd`, based on `947f1e4`.

All 12 host tests passed: seven C protocol interoperability tests against
Espressif's unmodified Security 1 Python client, and five factory reader/writer
interoperability, CRC rejection, identity-preserving update and output-safety
tests. These cover the protocol; the official phone app UI was not exercised.

Real BLE hardware verification used the same Espressif client over Bleak:

- Security 1 mutual proof succeeded using the factory-derived PoP.
- Encrypted HaLow scan completed with two AP results.
- Approved local WPA3 credentials were sent through the authenticated session.
- DHCP assigned `192.168.12.112`; successful status was reported only after
  the Settings/ZMS write completed.
- Endpoint `halow-5037cdf35bb6` registered to HaLowLink2 Wakaama. Resource
  `/5001/0/2` returned CoAP 2.05 with zero image length, and `/5001/0/5`
  returned the full 65536-byte cache capacity.
- Software reset reloaded saved credentials without starting BLE, and a live
  LwM2M resource read succeeded afterward.
- Operator held and released the physical button for at least five seconds.
  Saved/active flags became false and BLE advertising reopened. A second
  authenticated scan/provision/save cycle succeeded with the same factory PoP.

The initial prototype could authenticate but failed on short encrypted
messages. Mbed TLS rejects in-place cipher updates for a non-block-sized
length, including CTR mode; using a separate output buffer fixed this on
hardware. macOS cached the previous firmware's peripheral name, so the live
helper matches the current advertisement's local name instead.

After successful provisioning, SWD snapshots confirmed the BLE window closed,
`bt_started=0`, settings committed, camera UART22 ENABLE=0, SPIM00 ENABLE=0,
SAADC ENABLE=0, camera power low, and image length zero. Logging, console and
RTT remain disabled; dynamic PS remains 13 ms, listen interval 0, and health
check 600 seconds. AP beacon, 1 MHz bandwidth and DTIM10 were not changed.
No Capture execute was sent during these tests.

This run does not establish a new power measurement. J-Link remained attached
for state inspection; a disconnected-SWD cold-start meter comparison against
the approximately 3 mW baseline remains necessary. Factory-first Wi-Fi
precedence is implemented; the hardware run intentionally used empty factory
Wi-Fi to test the BLE-saved path. Factory-page readback, binaries and private
provisioning logs are saved locally under ignored `artifacts/ez-cam01-ble-prov/`.

## Pre-first-capture UART sleep regression — 2026-11-24

Operator measured approximately 4 mW before capture and approximately 3 mW
once capture completed and UART closed. This points at the startup/capture
lifecycle; it does not establish that BLE advertising remains active. The
previous ENABLE=0 snapshots alone did not prove the reported power floor.

Prepared a UART-only lifecycle test in the BLE camera profile: UART22 uses
`zephyr,deferred-init`, so its driver is not initialized at system boot.
Startup only parks TX/RX in sleep pinctrl and disables camera power. The first
Capture initializes the UART once, then uses the existing capture and shutdown
sequence. The non-BLE baseline retains its eager initialization behavior.

Clean build passed in 188.72 seconds, with 960668 flash bytes and 249310 static
RAM bytes. Generated devicetree confirms deferred init on UART22. Test HEX/ELF
and logs are saved under ignored `artifacts/ez-cam01-uart-deferred/`.
J-Link sees approximately 3.3 V but SWD attach failed. This image has not yet
been flashed, and no improved power result is claimed. Next verification is
a cold start without taking a photo, followed by normal Capture and shutdown.

## On-demand IMU / voltage and native IMU Observe — 2026-11-24

Added IPSO accelerometer 3313, gyrometer 3334 and voltage 3316. Carrier netlist
confirms SDA P0.04, SCL P0.03, IMU address 0x6a, battery divider EN P1.14 and
battery input P1.13/AIN6. IMU VDD/VDDIO are hardwired to 3.3 V, so idle means
both ODRs in power-down, not a physical rail cut. The ADC divider is switched.

The combined BLE/camera/sensor build passed in 166.93 seconds: 970104 flash
bytes, 250039 static RAM bytes. The standard camera environment also built
successfully in 121.45 seconds. Ten native object/Observe lifecycle cases
passed under AddressSanitizer and UndefinedBehaviorSanitizer. They cover
on-demand/metadata/Discover, attribute-only watchers, change/no-change,
multiple subscriptions, cancellation, registration/context loss and read
failures. These tests use a fake board, not simulated analog measurements.

SWD recovered on the next attach attempt. The combined BLE image was then
programmed and verified on probe 607000398. Before any Capture, snapshots
showed camera ready=1, UART configured=0, JPEG length=0, BLE stopped, UART22
ENABLE=0, SAADC ENABLE=0, TWIM30 ENABLE=0, observation mask=0 and successful
IMU standby initialization. P1.14 divider EN and P1.09 camera EN were low.

Real single-resource reads and native Observe succeeded over HaLowLink2.
Two Observe subscriptions (3313/0/5702 and 3334/0/5702) produced changing
measurements, then both were cancelled. After cancellation, software stream
and IMU ODR masks were zero, I2C/ADC/UART remained disabled, divider EN stayed
low, and image length remained zero. No photo was taken. SWD memory/register
checks do not constitute a new standby-power measurement.

The live tests exposed a separate server TLV typing defect: float payloads
were shown as integer bit patterns and the string V as integer 86. Server fix
8086cf2 uses the Web UI's XML resource model types; package deployment is being
validated separately. Instance-level Read on the current gateway REST API
returns not_implemented; use individual resource reads for this gateway.
Private build binaries and logs are saved under ignored
`artifacts/ez-cam01-sensors/`. See SENSORS.md for lifecycle and object paths.

The operator then disconnected J-Link, fully power-cycled the board without
a Capture or Observe subscription, and confirmed a flat sleep floor of
**3 mW** ("3mW, works well"). The combined sensor/BLE firmware therefore
restores the pre-first-photo low-power condition; this is a flat sleep-floor
measurement, not an average including DTIM wakes or active IMU observation.

## Standard Device object — 2026-11-24

Replaced the example Device object and removed the standalone Voltage 3316
object. Device /3/0/7 returns source-instance 0 in mV and /3/0/9 follows the
operator policy: 3.0–4.2 V estimates 0–100%, above 5 V is 100%, with clamping.
Factory identity, firmware/Zephyr versions, heap/SRAM figures, error state,
time metadata and delayed Reboot/Factory Reset commands use real platform
state. Current measurement and ExtDevInfo are absent because no corresponding
hardware/link exists. Battery charging status remains Unknown.

Fifteen native scenarios pass with AddressSanitizer/UndefinedBehaviorSanitizer.
The BLE firmware compiled successfully (970236 flash / 250248 static RAM bytes
before the final portable identifier/validation cleanup). Gateway HTTP/CoAP
regression checks pass for float/string/bool/integer and Device multi-instance
TLV values; Observe SSE preserves the same model types and instance IDs.
Gateway fix b8b4332 is published and its installation package is building.
This Device revision has not yet been flashed or measured; the confirmed
3 mW result above belongs to the prior combined sensor image.


The final pre-clock Device image (970240 flash / 250248 static RAM bytes) was
programmed and verified. Live registration contains Device 3, accelerometer
3313 and gyrometer 3334, with no Voltage 3316. Real reads returned factory
serial FGH200M-5037CDF35BB6, Zephyr 4.4.0, battery estimate 22%, free heap 35 kB,
SRAM 262 kB and Unknown charging status. The old gateway still mis-types some
TLV strings/multiple resources; the corresponding package is built separately.
After reads, SWD confirmed UART22/SAADC/TWIM30 disabled, camera/divider EN low,
no image and no IMU Observe/hardware mask. Standard non-BLE image also builds
(855644 flash / 248927 static RAM bytes). No Capture was invoked in this run.

Automatic clock sync reuses the existing registration and update ACK JSON,
adding full UNIX milliseconds while preserving relative time/sampling fields.
Client accepts only matching pending registration transactions and applies time
only after Wakaama reports successful registration. Nineteen native cases pass
using the actual CoAP parser and ASan/UBSan. Real HTTP/CoAP integration verifies
UNIX time in both registration and renewal ACKs. Web time values remain UNIX
seconds and are converted to browser milliseconds for display. The clock sync
firmware/gateway pair is still undergoing hardware deployment validation.


Gateway package `0.1+9d4525cc82b2-1` is deployed on HaLowLink2. The UCI config
SHA256 remains 097318fa71ec467f5b53280299bc02b69ad94141b804599a67fad3c4c69fa7a4;
DTIM remains 10. Real reads now show Manufacturer EdgeZ.ai, Model EZ-CAM01,
Device voltage as integer mV in a resource-instance map, and IMU floats.
A subsequent native IMU Observe run produced 15 typed FLOAT SSE events and
both subscriptions returned cancelled=true. No subscription was left active.

The first clock matcher incorrectly inspected URI option nodes after Wakaama
serialization had freed them. Matching the preserved serialized request buffer
fixed it; native tests now reproduce that serialization step. Real /3/0/13
then returned valid UNIX time without a manual Write, approximately 1.7 seconds
behind the host read completion. Standard Current Time Write/Read and Reboot
Execute had also passed on hardware. Time metadata adds no polling thread.

Voltage checks: R26 and R29 are each 100 kΩ; R27 is the EN pull-down. The ADC
measures the TP4054 BAT/VBAT+ net, not USB_5V. A 100 ms test showed readings near
2910–2916 and 3272–3280 mV; longer acquisition, discarding first samples and
turning off per-read calibration did not remove the variation. A divider-only
500 ms delay also returned about 2896–2914 mV. SAADC NOISESHAPE read back zero.
The operator reported 3.3 V supply and later 5.149 V USB / 4154 mV reading.
USB input is not a valid direct reference for the BAT net.

The operator explicitly selected a fixed ×1.13 correction, overriding the
proposal to keep unity pending a direct VBAT measurement. Final profile keeps
100 ms settling after actual SAADC enable, applies ×1.13 to Device voltage and
uses that corrected voltage for Battery Level. Build/flash verification of this
last profile is pending. A fresh disconnected-SWD standby-power measurement
has not been made for the Device/clock/voltage-calibration revision.


Final ×1.13 / 100 ms firmware was built, programmed and verified. Initial
field testing found a second clock issue: the ACK JSON crosses the gateway's
256-byte Block2 boundary once uptime gains digits, leaving the closing JSON
brace out of block 0. Packet capture confirmed this exact first-block shape.
The client now retains the complete timestamp field from block 0 and applies
it only after Wakaama accepts registration; Wakaama still owns block transfers.
Pending metadata expires after 60 seconds and is cleared on context teardown.
Twenty-two native cases cover serialization, fragment completion, expiry and
context reset; all fifteen Device/sensor lifecycle scenarios also still pass.

Real Current Time Read succeeded after flash and again following standard
Device Reboot Execute (2.04). The second boot returned 2026-11-24 19:31:24 UTC
without a manual time Write. Real corrected voltage read about 3286 mV, but the
underlying reading variation remains; the multiplier is operator-selected.
Post-read SWD checks confirmed UART22, SAADC and TWIM30 ENABLE=0, BLE stopped,
no image, zero IMU stream/hardware masks and zero sensor I/O errors. Camera and
divider EN were low. No Capture or Factory Reset Execute was performed.


## Local clock RTT correction and Time Observe — 2026-11-24

Both camera environments compile with local ticking and an eight-sample
registration RTT mean. Retransmission ambiguity is excluded. Clock Observe
runs a one-second change check only while a live watcher exists. Sixteen native
Device/sensor scenarios and twenty-five clock/RTT cases pass under ASan/UBSan.
Hardware's clock advanced from 1795549435 to 1795549439 between two reads without
a time-sync request. A successful Observe run then delivered eight consecutive
integer values 1795549439 through 1795549446 and cancellation returned true.
Earlier rapid re-observation attempts timed out at the gateway twice; a later
start/cancel passed. No gateway fix for that intermittent behavior is claimed.
The final hardware RTT history contained two valid samples, 1096 ms and
171 ms: mean RTT 633.5 ms, with 316 ms integer one-way correction. Mean-window
replacement and retransmission exclusion were validated natively. Standard
Registration Update Trigger executes returned 2.04. After cancellation, the
clock Observe deadline read back zero and SAADC remained disabled.

Final BLE build: 972308 flash bytes / 250364 static RAM bytes. The corresponding
standard camera environment also built successfully. The final firmware retains
×1.13 voltage correction, 100 ms ADC-enabled settling, DTIM10 and the deferred
camera UART startup fix. No new physical standby-power measurement was supplied.

## Timed WNM Sleep — 2026-11-24

Object 31026 adds a duration Write and deferred Sleep Execute. Native ASan/UBSan
tests passed for duration limits, response grace, TX/transaction drain, AP
rejection, exit errors, link loss and Execute retransmission deduplication.
Both camera environments built. The existing sensor and clock suites and the
server's real HTTP/CoAP typed decoding integration passed.

The first hardware run (before the SDK exit fix) accepted Execute in 17 ms,
negotiated entry in 18 ms, slept 30000 ms and exited in 1327 ms. Immediate
post-wake reads/writes worked and the AP association timestamp was unchanged.
**This did not prove stable recovery:** subsequent quiet-period requests and
Observe timed out. Packet capture showed responses arriving only during later
uplink activity. Temporary AP neighbor entries used for diagnosis were removed.

The SDK source and real-FSM regression test reproduced the cause: hardware
restore reapplies chip WNM sleep while the FSM is ACTIVE, but the old powerdown
exit path failed to clear it. The fix explicitly clears it on both exit paths
and reacquires the restored VIF ID. The native SDK test fails before the fix and
passes after it, for both WNM variants and repeated cycles.

Server model/type table commit `d34e4bd` built successfully in GitHub Actions
run `45476678861`; its application-only IPK was installed on HaLowLink2. Existing
gateway config hash stayed
`097318fa71ec467f5b53280299bc02b69ad94141b804599a67fad3c4c69fa7a4`,
AP WNM support remained enabled, and DTIM stayed 10. No router firmware, kernel
or wireless configuration was changed.

With the SDK fix flashed, the strengthened 30-second test passed: Execute
acknowledged in 15 ms, entry 40 ms, sleep 30000 ms, exit 1299 ms, result 0.
Both immediate Read and a Read after another 12 seconds of silence succeeded,
then duration Write succeeded. A second 15-second cycle with an existing
Current Time Observe entered in 19 ms, slept 15000 ms, exited in 1282 ms and
returned result 0; six notifications arrived after wake without re-subscribing.
Observe was cancelled successfully and the sleep duration restored to 60 s.
The AP association was retained across both fixed cycles; its start timestamp
was `1795551569660` ms. No static neighbor entry remained. Both firmware
environments also rebuilt successfully with the SDK fix.

WNM power/current has **not** been measured. SWD was attached for functional
validation. No claim is made yet that this image beats the 3 mW DTIM baseline,
and long sleeps beyond DHCP/AP/registration lifetimes were not tested.

### Remaining issue after longer quiet time

A final check about three minutes after the corrected firmware's association
found a timeout again. This is distinct from the already-fixed chip WNM flag:
a temporary static neighbor on the AP's actual `br-lan` route immediately
restored a successful unicast Read with result 0. The entry was then removed.
Thus short-cycle/Observe tests passed, but reliable ARP/broadcast recovery after
longer quiet periods is **not yet established**. This is an experimental WNM
version, not a replacement for the validated ordinary DTIM10 baseline.
The remaining receive/ARP issue needs further diagnosis; no permanent AP ARP
workaround or DTIM change is installed.

## WNM sustained power regression — 2026-11-24

The operator reported approximately **100 mW throughout the middle of the WNM
sleep interval**, with J-Link disconnected. This is a sustained plateau, not
an entry/exit spike. The current WNM powerdown version therefore fails the
power objective; SDK success and network timeouts did not prove low power.

Code inspection found a Zephyr WLAN HAL shutdown polarity error. RESETn is
`GPIO_ACTIVE_LOW` on P1.04, but `mmhal_wlan_deinit()` wrote logical 0, which
drives physical HIGH and releases reset. The official MM8108 SDK HAL asserts
reset before disconnecting SPI/GPIO. The correction writes logical 1 (physical
LOW) before bus release. Rail power itself is not switched off. Startup and
hard-reset timing remain unchanged.

The real pinned HAL is compiled under native ASan/UBSan with GPIO logical-to-
physical polarity simulation. It fails before the fix and passes after it for
three init/reset/deinit cycles, asserting RESETn LOW during bus release and
HIGH after restart. The BLE camera environment builds successfully. This is
a confirmed code defect and a plausible cause of excess power; the amount of
physical power reduction has **not yet been measured** on the corrected image.

The RESETn-fix image was subsequently flashed and verified. During a 30-second
WNM cycle, SWD register inspection showed P1.04 configured as an output with its
output LOW, WAKE output LOW, camera EN output LOW and SPIM00 ENABLE=0. This
checks controller output configuration, not an independent voltage measurement.
After 30000 ms, wake completed with result 0; entry took 25 ms and exit 1277 ms.
Current Time Read and sleep-duration Write recovered. The operator then
confirmed SWD disconnected, cold booted and prepared the meter; a separate
60-second WNM measurement was triggered over LwM2M. Its power reading is pending.

The separate no-SWD cold-boot measurement cycle completed: cached sleep time
60000 ms, exit 1266 ms, result 0, state idle, one completed cycle, and cached
resources readable after wake. No CoAP requests were sent during the measurement
window. The operator's middle-of-sleep power value remains pending.

### Operator-confirmed WNM power result

The operator reported **approximately 1 mW** for the middle-of-sleep stable
plateau in the 60-second RESETn-fix trial, with J-Link/SWD disconnected and
after a full cold boot. The previous WNM plateau was approximately 100 mW under
the same disconnected-debugger measurement setup; the ordinary DTIM10 sleep
comparison was approximately 3 mW. This is a flat sleep-floor measurement, not
an average over WNM negotiation, wake, camera captures or image transfer.

The confirmed improvement follows the WLAN HAL's reset-polarity correction;
no DTIM, UART or logging changes were introduced. The independent SDK WNM-exit
flag fix remains pinned. Long-idle ARP recovery is still a separate open issue;
this power result does not establish that it is resolved.

## Connection watchdog review and device deployment — 2026-11-24

Reviewed native registration/update ACK tracking, staged context rebuild and
Wi-Fi reconnect, bounded retry backoff, WNM pause/wake handling and client
teardown. The Wakaama teardown fix is pinned at `04e18bb`; pending transaction
cancellation callbacks run before their server/Observe state is freed.

All 19 host tests passed, including the actual Wakaama core teardown and
watchdog policy tests under ASan/UBSan. Both `ez-cam01` and
`ez-cam01-ble-prov` built successfully. The default image uses 861308 flash
bytes and 249338 static RAM bytes; the BLE image uses 975744 flash bytes and
250643 static RAM bytes. The BLE image retains a 60-second probe interval,
45-second response deadline, WNM support and disabled RTT.

Ran the J-Link uploader on probe 607000398. Flash comparison found the reviewed
BLE image already matched device contents; the uploader then reset and ran it.
HEX SHA-256:
`3e3f9ce6f1c04e210e98e93a556f0a305388b87373b34b191c491024e46ef5b9`.
Startup registration and all eight watchdog diagnostic resources were readable.

A 15-second WNM cycle completed with result 0, entry 27 ms, exit 1301 ms and
15000 ms asleep. The existing context was retained with zero watchdog context
rebuilds or Wi-Fi reconnects. Subsequent resource requests intermittently timed
out; later, 16 consecutive reads succeeded without a reset. Gateway logs also
show a five-second API cooldown after timeouts. These observations do not
establish reliable sustained downlink delivery or resolve the earlier ARP issue.

Restarting the Wakaama gateway service recovered in 43.37 seconds, with the
context-rebuild counter increasing from 0 to 1. A temporary, camera-only
firewall rule then dropped CoAP uplinks for 165 seconds (11 packets / 1423
bytes). The watchdog escalated through another context rebuild and one Wi-Fi
reconnect. After automatic rule removal, registration and resource reads
recovered in 2.49 seconds. No credentials or AP settings were changed.

A 310-second WNM cycle outlived the 300-second registration. It slept for
310000 ms, returned result 0, and replaced the expired registration (context
rebuild counter 2 to 3; reason 1, expired). Entry took 18 ms and exit 1325 ms;
the first post-sleep health check completed in 1.08 seconds. The Wi-Fi reconnect
counter stayed at 1, so this wake used registration recovery without another
watchdog reconnect. Sleep duration was restored to 60 seconds.

After 125 seconds without API traffic, the client remained online and the
last successful ACK was eight seconds old, confirming automatic probes
continued. Context rebuilds stayed at 3 and Wi-Fi reconnects at 1. All final
watchdog resources were readable and the temporary firewall table was absent.
The live recovery suite passed; it recorded expected missing-client responses
during server restart and one resource-read timeout that succeeded on retry.
The earlier intermittent timeout observations remain relevant: this is recovery
validation, not a zero-packet-loss guarantee. No new physical power result is
claimed.
Build, flash, packet and live-test logs are local under
`/private/tmp/ezcam-watchdog-review-*`; firmware and collected logs are also
saved in ignored `artifacts/ez-cam01-watchdog/`.

## WNM-only signal-pin parking experiment — 2026-11-24

Added optional `CONFIG_EZ_CAM_WNM_PARK_PINS` and separate
`ez-cam01-wnm-pins` BLE camera build. The baseline profiles keep it disabled.
After successful SDK WNM powerdown, explicitly suspend host SPI and disconnect
SCK/MOSI/MISO/IRQ/BUSY with no pulls or input buffers. Keep RESETn/WAKE output
LOW and CS output HIGH. The HaLow supply remains enabled. Restore saved GPIO
configuration and output latches before SDK wake; do not change nrfx interrupt
allocations or normal DTIM operation. See the pin table in SLEEP.md.

All 21 native tests passed, including actual parking source under ASan/UBSan,
exact GPIO restoration, PM ownership and entry/exit error cleanup. The test
profile built successfully: 976432 flash bytes and 250725 static RAM bytes.
The original BLE profile also rebuilt with parking disabled: 975744 flash
bytes and 250643 static RAM bytes. That baseline image was not flashed over
the parking experiment.
HEX SHA-256:
`bf906335dad586550041f6ee6baa33327aa0527cd18e860e608347e39b82494a`.
J-Link probe 607000398 programmed and verified the image, then reset/run.

During real 30- and 15-second WNM cycles, SWD confirmed passive-pin PIN_CNF=2
(input disconnected, no pull, high impedance), control-output PIN_CNF=3,
RESETn/WAKE LOW, CS HIGH, SPIM00 ENABLE=0, camera EN LOW and module EN HIGH.
The passive diagnostic array showed `[1,1,0,0]` and `[1,2,1,0]` during sleep;
after wake it showed `[0,1,1,0]` and `[0,2,2,0]`. GPIO configurations returned
to their pre-sleep values. The BUSY input's normal pull-down was restored.

Both cycles returned result 0 and served Current Time reads afterward.
Entry/exit took 36/1287 ms and 26/1314 ms; slept times were 30000/15000 ms.
Final watchdog state was online with zero context rebuilds or Wi-Fi reconnects,
and image length remained zero. One diagnostic read timed out and succeeded
on retry. Sleep duration was restored to 60 seconds.

This establishes GPIO behavior and functional wake. SWD was attached for these
checks. The operator subsequently reported no measurable improvement with pin
parking: WNM still measured approximately 1 mW, compared with approximately
550 uW for the no-HaLow condition. Thus GPIO parking did not explain the
reported difference at the meter's resolution; it does not isolate module,
regulator or MCU contributions. No AP settings or credentials were changed.
Firmware and logs are saved under ignored
`artifacts/ez-cam01-wnm-pins/`.

Source/configuration inspection confirms the WNM application loop blocks on
eventfd/poll until its sleep deadline, `CONFIG_TICKLESS_KERNEL=y`, GRTC system
timing, and the ARM Zephyr idle path using WFI. Status logging and the optional
20 ms diagnostic timer are disabled. These establish an idle-capable design,
not measured CPU sleep residency or proof that all clocks/peripherals are idle.
System OFF on nRF54L15 supports a GRTC wake but resets the MCU; it cannot simply
resume the existing Zephyr/Wakaama/SDK contexts after the timed WNM interval.

## Base, camera, radar and on-demand GPS variants — 2026-11-24

Added four explicit UART-device build environments while retaining the legacy
camera profiles and default. All four include BLE provisioning, factory data,
Device/IMU objects, the HaLow UDP client, connection watchdog and timed radio
WNM. UART selection is mutually exclusive; GPS acquisition is on request and
powers the receiver off after a fix, timeout, error or cancellation. See
[VARIANTS.md](VARIANTS.md) for wiring and resource paths.

All four environments built successfully using the pinned local dependencies:

| Environment | Flash bytes | Static RAM bytes |
| --- | ---: | ---: |
| ez-cam01-base | 977760 | 183906 |
| ez-cam01-camera | 975744 | 250643 |
| ez-cam01-radar | 983288 | 190565 |
| ez-cam01-gps | 982620 | 184829 |

Generated configurations and ELF symbols confirm one selected UART device,
camera object only in camera, radar presence/distance only in radar, and GPS
objects only in GPS. Base disables SERIAL and has no UART driver symbols.
Radar selects asynchronous UART; camera and GPS use interrupt-driven UART.
All four disable LOG, CONSOLE and USE_SEGGER_RTT, and no RTT backend/buffer
symbols are linked. WNM signal-pin parking remains off by default and its
devicetree passive-pin list is now shared across the carrier variants.

All 23 native tests passed, including the actual GPS NMEA parser and LwM2M
object under ASan/UBSan. GPS tests cover checksum/status/date/coordinate
validation, cached reads without acquisition, timeouts, failed startup/read,
and WNM/context cancellation. Separate tests compile the actual GPS UART
boundary with fake driver APIs to verify deferred initialization, ISR-to-work
handoff, supply-before-UART startup, pin parking and power-off after driver
initialization, callback or resume failure. The GPS control XML and local
documentation links also passed parsing/link checks.

The variant matrix is build/host-test validation. No variant image was flashed
for this change, and no new GPS/radar hardware or power result is claimed.
Radar requires compatible autostart TLV 1101 firmware and remains powered
during radio-only WNM. The GPS server model is supplied as
`docs/lwm2m/31028.xml`; server installation is a separate step.

HEX/ELF files, per-variant hashes/configuration checks and build/test logs are
saved locally in ignored `artifacts/ez-cam01-variants/`.


## 2026-11-25 — EZ-DEV54 unified hardware image

Built `pio run -e ez-dev54` with all base/camera/radar/GPS backends linked.
Boot priority: saved Settings/ZMS override, valid factory extension, then base.
Object 31029 exposes persistent selection and deferred reboot. Factory v2
identity/PoP and saved Wi-Fi namespace stay compatible. UART22 uses deferred
initialization, runtime pinctrl/baud and only the interrupt-driven API.
RTT, console and logging are disabled in the generated configuration.

- Full native suite: **27 tests passed**. New sanitizer-backed cases cover
  precedence, persistence/reload, invalid records/types, failed writes, clear
  override, deferred activation, LwM2M callbacks and UART mode boundaries.
- Factory extension round trips match the firmware CRC parser; adding a
  hardware flag preserves the existing identity bytes.
- Linker allocation: FLASH 992040 bytes / 1392 KiB; RAM 258672 / 262144 bytes
  (98.68%, including static stacks and configured heap). RAM margin is tight.
- HEX SHA256: `4d5e255695d733071337f0d1ba323b8ea7df472b4aa7eddde1eb783ad20d42fc`. Local ignored artifacts: `artifacts/ez-dev54-unified/`.
- Build and host validation only. This image was not flashed and factory
  data was not changed. Physical peripheral behavior, runtime heap/stack
  headroom and power consumption still require on-device verification.


## 2026-11-25 — Allocate peripheral memory for the selected hardware

Built `pio run -e ez-dev54` after replacing the static JPEG cache and UART
byte arrays with a selected boot-lifetime workspace from Newlib's heap.
Base allocates no workspace; camera allocates 66565 bytes; radar allocates
1024 bytes (configured RX capacity); GPS allocates 512 bytes. Only radar
allocates its thread stack, using Zephyr's dynamic stack API/kernel heap.
The common radio/kernel heap reservation is unchanged.

- Linker static RAM: **186488 / 262144 bytes (71.14%)**, previously
  258672 (98.68%): **72184 bytes** freed from unconditional allocation.
- Static FLASH: 992312 bytes / 1392 KiB.
- The link leaves 75656 bytes for Newlib's heap before runtime allocations.
  Camera's workspace consumes 66565 bytes plus allocator overhead, so the
  lower static percentage does not represent total camera runtime usage.
- ELF checks confirm JPEG storage and radar stack symbols are 4-byte
  pointers, and all changed peripheral objects were compiled after source
  changes. RTT, console and logging remain disabled.
- **28 host tests passed**, including all mode workspace sizes, ownership
  rejection, image/RX boundaries, stable repeated initialization, allocation
  failure/retry and GPS allocation failure before touching UART/power.
- Camera storage remains stable for boot-long CoAP Block2 borrowing.
  Uninitialized camera free/serialization paths handle a NULL cache safely.
- HEX SHA256: `01016b49732e277610b73b9afde6dd1db9b3b8fcdeb9e7e5986183b6ec9a553b`. Ignored local artifacts:
  `artifacts/ez-dev54-hardware-memory/`.
- Not flashed. Actual camera capture/transfer, radar operation and shared
  kernel-heap/stack high-water marks still require on-device testing.

## LwM2M IMU wake — live validation, 2026-11-25

GitHub Actions built and tested firmware commit `22206e6` (run
`45592569712`). Downloaded artifact hashes matched; the configuration enabled
IMU wake and ZMS settings, retained 256 KiB SRAM, and disabled logging/RTT.
J-Link probe 607000398 programmed and verified the CI HEX, then reset/run.
No local compilation was used.

The HaLowLink 2 AP was upgraded to gateway commit `bb4dc06` from its successful
CI IPK. Its UCI configuration checksum remained unchanged, the Hello World
WASM module resumed, and the live model catalogue exposed resources 8..11.

Live device checks passed:

- Read/write configuration and rejection of mode 3, threshold 31 mg and angle
  61 degrees.
- Mode 2, threshold 750 mg and angle 70 degrees survived a real LwM2M Device
  Reboot Execute.
- Timer-only WNM returned success after 8000 ms with wake reason 1.
- With orientation wake armed, an undisturbed cycle returned success after
  8001 ms with wake reason 1, confirming timer fallback and IMU cleanup.
- With a 180-second timer and 60-degree orientation threshold, the user
  physically turned the device. It woke after 23179 ms with wake reason 3,
  idle state and last result 0; subsequent network reads succeeded.

Some first post-sleep REST reads timed out; subsequent reads recovered and
returned the timings/results above. This test establishes functional wake and
configuration persistence, not a current-consumption measurement or a new
validation of unprovisioned transport-mode System OFF. Motion-mode hardware
sensitivity was not physically exercised in this session.

Test settings were restored to timer-only mode 0, 500 mg, 60 degrees and a
60-second duration. The tested firmware remains on the device and the matching
UI package remains on the AP.

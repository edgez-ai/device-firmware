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

## Radio power saving

The default station image sets `CONFIG_EZ_DEV01_AUTO_SLEEP=n`: HaLow power
saving is disabled, the listen interval is 1 beacon, and the connected MCU stays
awake at its configured CPU frequency. The AP's DTIM setting does not put an
always-awake station into sleep. Diagnostics report `ps_req=0 li=1 mcu_ls=0`.

Set `CONFIG_EZ_DEV01_AUTO_SLEEP=y` to opt back into radio power saving with a
10-beacon listen interval and automatic MCU light sleep after provisioning.
BUSY/GPIO7 and WAKE/GPIO9 remain configured in either build so an explicitly
requested WNM sleep cycle is still supported. Before provisioning, companion
and provisioning lifecycle behavior is unchanged.

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

### Remote power-save diagnostics

Read `/3347/0/5750` on the existing serial-number LwM2M client. The
`v1 idle` report includes requested radio power save (`ps_req`), listen interval
(`li`), station state, cached RSSI, restart/drop counters and GPIO samples.
`wake`, `busy`, `irqlow`, and `quiet` are basis points (10000 = 100%);
-1 means there were no samples. `quiet` means WAKE and BUSY are low and
active-low IRQ is inactive. These are GPIO-state samples during CPU0 idle,
not electrical current measurements or firmware sleep counters. IRQ may remain
low while the transceiver sleeps, so `quiet` is only the strict combination
of pin levels; use WAKE/BUSY activity and AP station power-save status together
when assessing sleep behavior. The sampler
uses existing idle entries without a polling timer or SPI transactions.
Each read resets the sampling window; allow 20–30 seconds between reads.
The read itself generates radio traffic. AP DTIM must be verified on the AP;
`li` reports the station configuration (1 by default, 10 in automatic
power-save mode), not a measured AP DTIM.
Object 3347 also exposes USER button state (5500) and volatile press count (5501).

### MCU automatic light sleep

Automatic light sleep is disabled by default. With
`CONFIG_EZ_DEV01_AUTO_SLEEP=y`, the `ez-dev01` image enables it only after BLE
provisioning has finished and the HaLow station has an address.
The external radio keeps its association. Its command vetoes and WAKE output
hold an MCU no-light-sleep lock during radio work. BUSY-high wakes the MCU via
EXT1, independently of the driver's normal BUSY edge interrupt. USER-low wakes
via EXT0; RTC pin routing is restored after each attempt. IRQ is not used as a
wake source because it can remain low while the transceiver sleeps.

Radio RESET, CS, WAKE, SCK and MOSI retain their active output levels. MISO and
IRQ inputs have sleep-time input buffers and pulls disabled, then restore on
wake. The BUSY internal pull-down is removed because the radio drives the pin.
Flash/PSRAM and unknown board pins are not isolated. Whole-board GPIO isolation
and flash power-down are not enabled.

The same `/3347/0/5750` report adds `mcu_ls`, `ls_try`, `ls_ok`, `ls_pm_ms`,
`ls_win_ms`, and `ls_wake`. Counters reset per read. `ls_ok` requires a real
ESP-IDF wake cause; rejected attempts are excluded. `ls_pm_ms` includes sleep
entry/exit overhead, so it is not exact hardware residency or measured current.
`ls_wake` uses ESP-IDF wake-cause values (2=EXT0, 3=EXT1, 4=timer).

### One-shot WNM sleep (same object as EZ-DEV54)

HT-HC33 exposes `/31026/0` on its existing serial-number endpoint:

| Resource | Operation | Meaning |
| --- | --- | --- |
| 0 | Execute, empty arguments | Queue one WNM sleep cycle |
| 1 | Read/Write integer | Duration in seconds, 1–86400; default 60, RTC-retained for WNM wakes |
| 2 | Read | 0 idle, 1 pending, 2 sleeping, 3 waking |
| 3 | Read | Last result; 0 success, negative SDK status/error on failure |
| 4 | Read | Completed cycles in the current power/session (retained across WNM deep wakes) |
| 5, 6, 7 | Read | Entry, exit and actual sleep time in milliseconds |
| 8, 9, 10 | Read | IMU wake disabled (0), reserved thresholds 500 mg / 60 degrees; writes unsupported on HC33 |
| 11 | Read | Wake reason: 0 none/error, 1 timer, 4 USER button |

Execute returns 2.04 before WNM entry. After a two-second response grace period,
confirmable LwM2M transactions get up to ten seconds to drain. Retransmitted
CoAP Execute requests cannot queue another cycle. The duration is snapshotted;
settings cannot change while a cycle is pending. IP/ARP/DHCP transmit is gated
without taking down the association. For an explicit request, radio power save is temporarily enabled if needed.
The SDK then negotiates WNM with
`chip_powerdown_enabled=true`, matching EZ-DEV54. Rejected entry restores the prior radio policy. Successful MCU deep-sleep wake
boots into the normal configured awake policy.
Unsupported/rejected WNM returns to normal operation with the error in resource 3; there is no direct
firmware-command fallback masquerading as a negotiated sleep.

After negotiated WNM power-down completes, the HC33 waits for radio/transport
shutdown before parking GPIOs. RESET and WAKE stay low, CS stays high, SCK and
MOSI stay low, and the battery-divider enable stays low. These output levels
are held across MCU deep sleep. Other unused non-memory pads (including radio
inputs, console and unused headers) have drive, input buffers and pulls disabled.
Flash/PSRAM pads are left to ESP-IDF, and USER/GPIO0 remains a pulled-up RTC
wake input.

The MCU enters true deep sleep, with timer and USER wake. Wake reboots the MCU,
releases all GPIO holds before driver initialization, and reconnects/re-registers
using the saved Wi-Fi profile and the same serial endpoint. Sleep duration,
last result, timings, replay protection and completed-cycle count are retained
in RTC memory across these WNM wakes; cold/software resets clear them. Resource
6 measures boot-to-IP recovery on HC33, and resource 7 excludes MCU boot time.
Normal connected-state automatic radio/MCU sleep remains disabled after wake.
The AP must support WNM (`wnm_sleep_mode=1` in hostapd). Remote requests cannot
wake a radio in WNM; wait for the timer or press USER. No baseline build is added.

Using the gateway's existing REST API (`CLIENT_URL` is
`http://<gateway>:8088/api/clients/<serial>`):

```sh
curl -X PUT -H 'Content-Type: text/plain' --data '60' "$CLIENT_URL/31026/0/1"
curl -X POST --data '' "$CLIENT_URL/31026/0/0"
# After timer/button wake:
curl "$CLIENT_URL/31026/0/3"
curl "$CLIENT_URL/31026/0/7"
```

A successful Execute means the one-shot request was accepted. Check resource 3
after wake/failed entry for its actual result. Do not poll during the requested
sleep interval: the radio is deliberately unavailable.

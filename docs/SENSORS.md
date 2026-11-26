# Device information, battery voltage and IMU over LwM2M

Camera environments enable `EZ_DEV54_SENSORS`. The BLE camera image also
retains its deferred UART initialization test. Sensors are registered in the
existing Wakaama client alongside camera, light and button objects.

| Object/resource | Read result | Unit |
| --- | --- | --- |
| `/3313/0/5702`, `/5703`, `/5704` | Acceleration X, Y, Z | m/s2 |
| `/3334/0/5702`, `/5703`, `/5704` | Angular rate X, Y, Z | rad/s |
| `/3/0/7` | Power source voltage, resource instance 0, accounting for the 2:1 divider | mV |
| `/3/0/9` | Estimated battery percentage | % |
| IMU objects' `/5701` | Unit string; no measurement is performed | — |

Read `/3313/0` or `/3334/0` for three axes from a single acquisition. A direct
Read samples current hardware and returns the value after cleanup. Failed
acquisitions return CoAP 5.03 rather than a fabricated zero or a previous
sample. Discover, unit-only reads, invalid resource IDs and Write Attributes
do not enable the ADC or start an IMU sampling loop.

## Standard Device object /3/0

The custom Device object replaces the stock example identity and the separate
Voltage object 3316. It uses the standard [OMA Device definition](https://raw.githubusercontent.com/OpenMobileAlliance/lwm2m-registry/prod/3.xml).

| Resources | Behavior |
| --- | --- |
| 0, 1, 2, 3 | EdgeZ.ai, EZ-DEV54, factory serial (hardware ID fallback), firmware Git revision |
| 4, 5 | Execute reboot / reset saved provisioning; reset preserves factory identity and keys |
| 6 | Available source instance 0: internal battery/input rail (1) |
| 7 | Source instance 0: measured integer mV; ADC active only during Read |
| 9 | Linear estimate from 0% at 3.0 V to 100% at 4.2 V, clamped outside that range; above 5 V is 100% |
| 10, 21 | Free tracked Zephyr heap and total SRAM, in decimal kB; available memory is an estimate |
| 11, 12 | Error resource instance 0 and Execute reset/re-evaluate error conditions |
| 13, 14, 15 | Read/write UNIX time, UTC offset and IANA timezone metadata |
| 16, 17, 18, 19 | UDP binding, device type, carrier hardware and actual Zephyr version |
| 20 | Unknown (6): the carrier has no charging-status input |

There is no current sensor or external Device-object link, so resources 8 and
22 are absent. Voltage-based capacity is approximate, not a fuel-gauge reading.
The input measurement cannot independently identify USB versus battery supply.
Resources 6, 7 and 11 are multi-instance resources, with values keyed by `0`.
Reading 7 or 9 acquires voltage once; an aggregate Device Read also samples
only once. Metadata reads and Discover do not start the ADC.

Current Time is synchronized from `server_unix_ms` in the gateway's existing
registration/update ACK JSON, after matching the pending transaction and Wakaama
accepting it. No SNTP query, polling thread or extra radio round trip is added.
Time advances with MCU uptime between acknowledgements. The gateway needs a
valid system clock. Older gateways without this field leave Current Time at
5.03 until a standard LwM2M Write supplies UNIX seconds. Uptime and weekly
counters are not treated as a date. Time, UTC offset and timezone are RAM state;
a cold boot re-synchronizes time on registration. Setting timezone metadata does not alter the UNIX timestamp. Reboot
and provisioning reset are delayed 500 ms to allow the Execute response to leave.
Factory Reset clears saved BLE Wi-Fi only, preserving immutable factory data;
factory Wi-Fi, if present, still takes priority after restart. Error reset uses
the last hardware I/O result and does not itself trigger a measurement.

## IMU Read and Observe

A standalone Read enables the requested accelerometer or gyroscope, waits
for data-ready with a 300 ms bound, reads the axes, powers the IMU engines
down and suspends I2C. There is no periodic idle sampling thread or IRQ.
At boot the code only resets ODR/FIFO/interrupt settings to idle; it does not
take a measurement. This also handles a warm MCU reset while the IMU remains
powered.

A native LwM2M Observe subscription on an IMU measurement, instance or object
keeps the requested IMU engine running. The LwM2M thread checks for changes
every 1000 ms by default. `CONFIG_EZ_DEV54_IMU_OBSERVE_INTERVAL_MS` selects
100–60000 ms at build time. The IMU itself uses 12.5 Hz ODR, ±2 g acceleration
and ±125 degrees/s angular rate; conversion to the resource units happens on
read. I2C is suspended between read transactions even during observation.

Changes are passed to Wakaama via `lwm2m_resource_value_changed()`; Wakaama
handles CoAP notifications and the server's normal `pmin`, `pmax`, `gt`, `lt`
and `st` attributes. A server may set `st` to avoid notifications for sensor
noise. IMU Observe intentionally consumes more power than idle.

The application inspects active Wakaama watcher records on the same thread
that owns the client context. Inactive records created by Write Attributes
do not count as subscriptions. Removing one watcher preserves other active
watchers; removing the last one powers the IMU engines down. Link/context
loss or loss of a registered server also ends continuous sampling. No custom
MQTT publisher or side-channel notification protocol is added.

Internal Wakaama observation evaluation reads the cached samples rather than
starting extra acquisitions. Only active IMU observers create a sampling
deadline. Voltage has no continuous sampling loop: direct Read refreshes its
value; native observation evaluation may reuse the last successfully read
voltage. Use Read to request a current voltage measurement.

## Carrier wiring and power limits

Wiring is taken from the supplied desensitized carrier netlist and the
tracker's EdgeZ carrier overlay, rather than XIAO Sense defaults:

| Signal | Connection |
| --- | --- |
| LSM6DS3TR-C SDA / SCL | P0.04 / P0.03, I2C30, address 0x6a |
| IMU INT1 | P0.02, GPIO disconnected; no interrupt subscription |
| IMU VDD and VDDIO | Direct 3.3 V rail |
| Battery divider enable | P1.14, active high, SY6280 EN |
| Battery ADC input | P1.13 / SAADC AIN6 |
| Divider | 100 kΩ / 100 kΩ, measured value multiplied by two |

The carrier provides **no independent IMU power switch**. P0.01 is not
connected to IMU power on this carrier. Firmware uses the chip's documented
power-down state (ODR=0 for both engines), not a physical rail cut. Cutting
its supply completely would require a hardware change. The always-powered
I2C pull-ups are compatible with parking SDA/SCL as disconnected inputs.

The voltage divider is physically switched off outside a Read. ADC driver
initialization is deferred until the first voltage request. Each request
discards the calibration conversion, enables SAADC and waits 100 ms before
the measurement conversion (40 µs acquisition). It disables SAADC,
lowers P1.14, disconnects the analog GPIO input buffer and suspends the ADC
metadata state, including cleanup on error exits. No Zephyr global runtime
PM change is required.

The old power-off overlay is applied before the on-demand overlay. The latter
must be last so ADC and I2C device nodes are enabled while their physical
measurement functions remain off between requests. The built-in Zephyr
LSM6DSL sensor driver is disabled to prevent automatic ODR/FIFO/trigger setup.

Reference object definitions: [Accelerometer 3313](https://raw.githubusercontent.com/OpenMobileAlliance/lwm2m-registry/prod/3313.xml),
[Gyrometer 3334](https://raw.githubusercontent.com/OpenMobileAlliance/lwm2m-registry/prod/3334.xml),
[Device 3](https://raw.githubusercontent.com/OpenMobileAlliance/lwm2m-registry/prod/3.xml).
Chip mode reference: [ST LSM6DS3TR-C application note](https://www.st.com/resource/en/application_note/an5130-lsm6ds3trc-alwayson-3d-accelerometer-and-3d-gyroscope-stmicroelectronics.pdf).

## Validation

`python3 tests/test_sensors.py` compiles the real LwM2M object/Observe module
with a fake board and AddressSanitizer/UndefinedBehaviorSanitizer. Fifteen cases
cover on-demand Read, metadata/Discover, invalid paths, attribute-only
watchers, changed/unchanged samples, multiple observers, cancellation,
registration loss, context teardown, failed samples, whole-object Observe, Device metadata, multi-instance voltage,
battery thresholds, clock writes, reboot dispatch and aggregate Read coalescing.
This verifies lifecycle policy, not analog calibration or physical power.

Hardware validation passed for flash/verification, startup UART/ADC/I2C and
divider-off state, real IMU/voltage reads, native Observe notifications and
cancellation cleanup. The operator confirmed a 3 mW flat sleep floor with
J-Link disconnected after cold start, before any Capture or Observe. See
VALIDATION.md for build and test evidence.

The Device-object revision has been programmed and verified, registered without
3316, and returned real identity/battery/memory resources. Automatic
time synchronization has been verified with gateway package 9d4525c.
`python3 tests/test_time_sync.py` additionally covers 19 matching/malformed
registration response cases against the real Wakaama CoAP parser, with ASan/UBSan.


Divider verification uses R26=100 kΩ from U11 OUT (`VBAT_ADC_CHECK`) to the
ADC node and R29=100 kΩ from that node to GND. R27=100 kΩ is the U11 EN
pull-down and is not a divider arm. The 2:1 conversion is therefore correct;
3.3 V input should produce 1.65 V at P1.13. On-device measurements have shown
two groups near 2.91 V and 3.27 V, so analog accuracy is not yet established.
Longer acquisition/discarding samples and disabling per-read calibration did
not remove the variation. Noise shaping was confirmed disabled, so Nordic
erratum 111 is not established as its cause. A 500 ms divider-only delay also failed to eliminate low readings; the selected
configuration keeps the operator-requested 100 ms delay after actual ADC enable.


The measured net is **VBAT+**, connected to charger U3 TP4054 BAT and the battery
connector CN2 pin 1, via U11. It is not USB_5V. USB reaches the charger through D4;
USB input and battery voltage must not be used interchangeably when calibrating.
For example, 5.149 V USB input and roughly 4.154 V at BAT may be normal. A direct
VBAT+ measurement against board GND is the reference for physical calibration.

`CONFIG_EZ_DEV54_VOLTAGE_SCALE_PPM` adds an optional build-time calibration multiplier
after the physical 2:1 divider correction. Default `1000000` is ×1.0, with a
range of ×0.5–×2.0. Use `round(actual_VBAT_mV / reported_mV * 1000000)` only after
the two values refer to the same net and stable supply. For a verified 3300 mV
VBAT and 2912 mV report, the single-point value would be `1133242`; this is an
example, not a deployed calibration. A charger-induced drop must not be scaled
away. Both Device voltage and its battery percentage use the corrected value.


The sensor profile uses unity calibration,
`CONFIG_EZ_DEV54_VOLTAGE_SCALE_PPM=1000000`. The former ×1.13 multiplier
is removed; the physical R26/R29 divider correction remains ×2.
No extra fixed gain correction is applied. The Kconfig fallback
remains unity for other profiles. The selected delay is 100 ms, not 500 ms.


## Circuit analysis: voltage error versus measuring different rails

Verified against the supplied `check_circuit_desensitized.json` netlist:

- U11 IN is VBAT+, OUT is VBAT_ADC_CHECK, and EN is P1.14.
- R26=100 kΩ connects OUT to MCU_ADC_VBAT/P1.13/AIN6; R29=100 kΩ
  connects that node to board GND. R27=100 kΩ pulls EN to GND.
- U3 TP4054 BAT connects to VBAT+; USB_5V reaches its VCC through D4.
  The USB connector voltage and the measured battery rail are different nets.
- CN2 pin 2 is VBAT−. U5 XB5606AJ connects its GND pins to VBAT− and VM
  pins to board GND; these grounds are separated by the protection path.

The nominal division is exactly 1/2. Even opposite 1% tolerances of R26/R29
produce only about ±1% voltage error, not the roughly 12% low group observed.
The ADC measures VBAT+ relative to board GND. A meter across CN2 pins 1/2
measures VBAT+ relative to VBAT−. If board GND is elevated by 0.388 V over
VBAT−, those readings differ by exactly 3.300−2.912=0.388 V. Such a large ground
difference would merit investigation of protection state, return wiring and
external USB/J-Link ground paths; it is a hypothesis, not a measured U5 fault.

The source resistance is R26||R29=50 kΩ. Nordic's acquisition-time table allows
100 kΩ at 10 µs; the selected 40 µs has additional margin. Therefore 100 kΩ
resistors alone do not establish an acquisition-time failure. The netlist has
no filter capacitor at MCU_ADC_VBAT and no capacitor at U11 OUT. A future
board test could add 10–100 nF from the ADC node to board GND to reduce RF/switch
noise (50 kΩ gives 0.5–5 ms RC, compatible with the existing 100 ms delay).
Silergy's reference circuit also specifies input/output decoupling for U11.
Normal switch on-resistance should not cause a large drop at divider current
of only about 16.5 µA for a 3.3 V input.

First compare two always-accessible meter readings under the same supply:
VBAT+ to board GND, and board GND to CN2 VBAT−. Then compare U11 OUT and
P1.13 to the same board GND while the divider is enabled. Expected values
for an actual 3.3 V VBAT-to-board-GND input are approximately 3.3 V at U11 OUT
and 1.65 V at P1.13. An oscilloscope/hold-enabled diagnostic is needed to inspect
these switched nodes reliably during the normal 100 ms measurement window.
If the physical ADC node is correct but the unscaled digital reading is wrong,
focus on ADC conversion/reference configuration. If the physical node is low,
inspect supply/protection/switch/divider/assembly rather than compensating it
as an ADC gain error. A fixed calibration multiplier does not remove the raw
reading's two-group variation.

References: [Nordic acquisition time](https://docs.nordicsemi.com/r/bundle/ps_nrf54l15/page/saadc.html-concept_qh3_spp_qr),
[Silergy SY6280/SY6280A](https://www.silergy.com/zh/download/downloadFile?ftype=note&id=4369&type=product).


## Local clock, delay correction and Current Time Observe

Device Current Time uses a UNIX-millisecond anchor plus `k_uptime_get()` elapsed
time, so it continues to tick locally between server responses and during link
loss. Protocol timers continue using monotonic uptime. Cold boot obtains a new
anchor from the gateway; there is no battery-backed calendar clock assumed.

Successful registration/renewal sends record their token, message ID and local
send time. Matching ACKs contribute to the arithmetic mean of the most recent
eight valid RTT samples. Ambiguous retransmissions and samples outside 0–60 s
are excluded. The client adds half the current mean RTT to the server timestamp,
then advances it by local elapsed time while native Wakaama processes any
remaining blocks. This is an approximate one-way-delay correction: DTIM and
asymmetric uplink/downlink delay can leave residual error. No periodic probe
traffic is added solely to measure RTT.

An active native Observe on `/3/0/13`, `/3/0` or `/3` creates a one-second change
check. Changes go through `lwm2m_resource_value_changed()` and Wakaama's normal
pmin/pmax handling. Cancellation, loss of a live registered watcher or context
teardown removes this deadline. With no Time Observe there is no extra periodic
clock wake-up; reads calculate the current time directly from the low-power
uptime counter. Clock notifications do not acquire ADC/IMU measurements.

Native validation now includes 16 Device/sensor scenarios and 25 clock/RTT
scenarios. Hardware produced consecutive integer time notifications after the
new firmware was flashed, and cancellation was exercised. A rapid re-observation
attempt twice encountered gateway REST timeouts; a later start/cancel cycle
passed. This intermittent gateway behavior remains recorded rather than being
claimed as resolved by the local clock change.

## Unprovisioned transport standby

The normal ODR-off/disconnected-IRQ policy above applies to configured devices
without active sensor observers. Unprovisioned System OFF instead leaves the
accelerometer in 26 Hz low-power mode with a 6D orientation interrupt on P0.02;
the gyroscope is off. An orientation wake starts a bounded, gyro-only
verification: at least two axes must simultaneously exceed the factory angular
rate threshold (30 degrees/second by default) for 100 ms within three seconds.
Every verification exit turns the gyro off. Invalid gestures rearm accelerometer
orientation wake; successful gestures open BLE provisioning. No periodic MCU
polling, NVS write or application RAM retention is needed during System OFF.
See [BLE provisioning](BLE-PROVISIONING.md#transport-standby-and-two-stage-provisioning-wake)
for timings, factory settings and physical transport testing requirements.

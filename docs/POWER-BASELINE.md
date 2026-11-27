# Archived nRF54L15 power measurements

The temporary all-off, WNM, DTIM radio-only and System ON idle environments
were removed from the active firmware at the user's request after testing.
The sections below preserve historical setup and results; their build
commands no longer apply to the current tree. The initial baselines are
recorded in commit `d6297e0`. Final measurement source snapshots, verified
HEX files and diagnostics are retained in local ignored artifacts.
Use the normal `ez-dev54` environment for application firmware.


`ez-dev54-all-off` is an independent image for the EdgeZ E73 carrier. It does
not initialize the normal application, HaLow driver, BLE, networking, logging,
UART, ADC, or storage. Only the IMU I2C bus runs briefly to put the permanently
powered LSM6DS3TR-C accelerometer and gyro into power-down, disable FIFO and
motion interrupts, and verify both ODR fields. The bus is then suspended.

HaLow and external camera/GPS/radar supplies are held off; battery-divider
gates are low, the LED is off, and known signal pins use disconnected inputs
without pulls or wake sense to avoid powering an unpowered peripheral through
its IO. After a three-second inspection interval the kernel timer is disabled
and the MCU enters System OFF without application wake sources.

```sh
pio run -d firmware -e ez-dev54-all-off
python3 firmware/scripts/flash_jlink.py \
  firmware/.pio/build/ez-dev54-all-off/firmware.hex --serial YOUR_PROBE_SERIAL
```

Only the application region below `0x15c000` is programmed. Factory identity,
keys, and saved Wi-Fi settings are preserved. Reset or a power cycle exits
System OFF, but the same baseline immediately shuts down again. Restore the
normal `ez-dev54` image to resume operation.

## Measurement

1. Record the existing normal-firmware measurement first.
2. Flash the baseline and, if validating registers, break at `sys_poweroff`
   to inspect `baseline_imu_error`, `baseline_bus_error`, the IMU identity and
   ODR readbacks, and rail GPIO latches. The expected errors and ODRs are zero.
3. Disconnect the SWD signals/probe from the target, keep PPK as the only
   supply, then power-cycle the PPK output. Connected debug can prevent true
   System OFF and invalidate the current reading.
4. Wait at least five seconds, then measure a stable interval. Record supply
   voltage, average current, maximum current, interval length and board wiring.
   This is the complete board's leakage floor, not an MCU-only specification.

This image intentionally stops online/LwM2M service. Re-enable one subsystem
at a time in later comparison profiles to attribute the current difference;
compare equally long intervals and include periodic HaLow traffic when
measuring the normal online firmware.

## HaLow WNM chip-powerdown comparison

Build `pio run -d firmware -e ez-dev54-wnm-baseline`. This independent image
uses the local ignored `halowlink2-secrets.conf`, associates in the EU region,
enables Wi-Fi power save and requests WNM with chip powerdown. It does not
start BLE, DHCP, LwM2M, MQTT or sensor application services. The permanently
powered IMU is put into power-down and its I2C bus is suspended.

Only after the AP accepts WNM does it suspend radio SPI and park its signals
using the existing WNM pin helper. The radio supply stays enabled. RESETn and
WAKE are held low, CS high; SPI data/IRQ/BUSY are disconnected with no pulls.
The MCU then enters System OFF with no timed wake. Power cycle or reset to
restart; restore the normal firmware to resume application operation.

Passive SWD values `wnm_baseline_join_result`, `wnm_baseline_ps_result`,
`wnm_baseline_entry_result` and `wnm_baseline_park_result` must all be zero,
and `baseline_stage` must reach 3. Failure leaves stage 0xee and never claims
the WNM floor. Disconnect SWD and power cycle before taking PPK measurements.
The AP must support WNM. Factory and saved settings are not modified.

## HaLow-only DTIM comparison (MCU System OFF)

Build `pio run -d firmware -e ez-dev54-dtim-baseline`. This environment uses
HaLowLink2 in the EU region, follows the AP's DTIM and sets the same 13 ms
dynamic idle timeout as normal firmware. BLE, RTT, logging, DHCP/IP, MQTT,
LwM2M and application reports are absent. It does not request WNM.
Confirm the gateway's actual DTIM configuration separately; a station cannot
impose AP DTIM10. In the measured setup, the running gateway configuration
had beacon_int=100 TU and dtim_period=10, giving a 1.024-second DTIM period.

The DTIM environment defines `HALOW_RADIO_ONLY`. After association
and enabling DTIM power save, it waits for WAKE=LOW and BUSY=LOW for 20 ms
(timeout 30 s), stops scheduling, suspends SPI, disconnects SPI/IRQ/BUSY,
keeps physical RESETn=HIGH, WAKE=LOW, CS=HIGH and radio power enabled, then
puts the MCU into System OFF. No host task runs during measurement.
Successful state is `baseline_stage=5`, `dtim_radio_quiesce_result=0`,
`dtim_radio_spi_enabled=0`, `dtim_radio_pin_levels=13`. Failure is stage 0xef.

This is a radio-autonomy experiment, not an operational online client: the
host cannot drain received frames, process WPA rekeys or recover association.
Check whether periodic beacon wakeups continue, and whether host-bound
traffic eventually holds BUSY asserted. Do not equate a low reading alone
with proof that the radio is still associated. The previous host-idle image
is retained in local artifacts for comparison.

## Observed measurements (27 November 2026)

PPK2 source meter at 3.7 V; user disconnected target SWD and power-cycled
before taking the stable windows shown in Power Profiler. Values measure the
complete carrier, not an isolated radio rail. Baseline images preserve factory
and stored settings; network secrets and binary artifacts remain local/ignored.

| Variant | Window | Average current | Average power |
| --- | --- | --- | --- |
| All off, MCU System OFF | 1 s | 36.54 uA | 0.135 mW |
| WNM chip powerdown, MCU System OFF | 3 s | 119.08 uA | 0.441 mW |
| DTIM with host idle and ordinary SPI driver | 3 s | 3.12 mA | 11.54 mW |
| DTIM radio only, MCU System OFF, SPI isolated | 10 s | about 511 uA | 1.89 mW |

The radio-only DTIM waveform showed narrow pulses spaced about 1.024 s apart,
consistent with the configured DTIM period. The prior host-idle run showed
additional, longer activity plateaus. Different window lengths and host/radio
interaction mean the current difference cannot be attributed entirely to MCU
consumption. Sustained association and recoverable operation were not verified
in the MCU-off experiment. Local artifacts retain the earlier host-idle image;
it is not a separate current PlatformIO environment.

## System ON idle comparison with identical radio pins

Build `pio run -d firmware -e ez-dev54-dtim-idle-baseline`. It inherits the
radio-only DTIM setup unchanged, including RESETn HIGH, WAKE LOW, CS HIGH,
radio power HIGH and disconnected SPI data/IRQ/BUSY. After the exact same
quiesce sequence, it replaces System OFF with shallow CPU WFI, retaining
MCU/RAM. The scheduler, system clock and host interrupts remain stopped to
avoid background tasks using the isolated bus. No periodic wake is enabled.
Reset or power-cycle to restart. This remains an isolated measurement image,
not a recoverable online application; BUSY/IRQ wake has not been restored.

Verify `baseline_stage=6`, `dtim_radio_spi_enabled=0`, and
`dtim_radio_pin_levels=13`, with all association/PS/quiesce results zero.
Expected stack is the shallow WFI loop in main, not `sys_poweroff`. Compare
against the radio-only System OFF measurement using equal windows and the
same supply voltage.

The rightmost stable 10-second recorded window showed 528.80 uA average,
63.19 mA maximum, or 1.957 mW at 3.7 V, with approximately one large pulse per
second. The prior System OFF 10-second window was about 511 uA. The observed
difference of about 18 uA is small, but separate-run radio pulse energy and
measurement variation prevent treating it as an isolated MCU consumption.

### Input-voltage observations, System ON idle

PPK2 connected to the board power input. Each displayed stable window was
10 s after a power cycle; these are single-run observations, not voltage
rating or regulator-fault conclusions.

| Input | Average input current | Input power | Observed pattern |
| --- | --- | --- | --- |
| 3.3 V | about 1.67 mA | about 5.5 mW | 1 s pulses, elevated floor |
| 3.4 V | 18.18 mA | about 61.8 mW | Sustained activity; diagnostic stage not read |
| 3.5 V | about 532.5 uA | 1.86 mW | Narrow approximately 1 s pulses |
| 3.7 V | 528.80 uA | 1.96 mW | Narrow approximately 1 s pulses |
| 3.8 V | about 529 uA | 2.01 mW | Narrow approximately 1 s pulses |

The nonmonotonic 3.3/3.4 V behavior requires repeated runs and passive stage
checks before attributing failure to input voltage. Board rail measurements
are needed to distinguish supply behavior from SDK/association/quiesce state.

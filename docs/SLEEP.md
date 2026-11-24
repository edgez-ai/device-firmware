# Timed WNM deep sleep

**Experimental:** short sleep/wake cycles work, but longer quiet periods can
lose ARP reachability after chip powerdown. A temporary AP neighbor bypass
restored unicast reads during diagnosis; it was removed. See
[validation evidence](VALIDATION.md#remaining-issue-after-longer-quiet-time).
The ordinary DTIM10 baseline remains the known-working comparison.

The camera firmware exposes vendor object **31026**, instance 0. The AP stays
at DTIM10. Sleep uses the pinned EdgeZ SDK's
`mmwlan_set_wnm_sleep_enabled_ext()` with `wnm_sleep_enabled=true` and
`chip_powerdown_enabled=true`. It does not call `mmwlan_shutdown()`, disable
the station, or intentionally disconnect from the AP.

During WNM sleep the MM8108 skips DTIM reception. This is an additional,
AP-negotiated sleep period, not a way to keep receiving every DTIM10 beacon
while powered down. On timer expiry, the SDK exits WNM and normal DTIM power
save resumes. The nRF54L15 retains RAM and waits in its normal tickless idle;
this is not nRF System OFF.

| Resource | Access | Meaning |
| --- | --- | --- |
| /31026/0/0 | Execute | Request one sleep interval, no arguments |
| /31026/0/1 | Read/Write | Sleep seconds, 1–86400, default 60; RAM until reboot |
| /31026/0/2 | Read | 0 idle, 1 pending, 2 sleeping, 3 waking |
| /31026/0/3 | Read | Last result: 0 success, positive SDK status, negative errno |
| /31026/0/4 | Read | Completed sleep/exit cycles since boot |
| /31026/0/5 | Read | Last entry negotiation time, ms |
| /31026/0/6 | Read | Last exit negotiation time, ms |
| /31026/0/7 | Read | Last actual sleep interval, ms, excluding negotiation |

This is an EdgeZ vendor object, not an OMA-registered standard object. Its XML
model is in `wakaama-server/web-ui/models/31026.xml`, so the generic UI can
Read, Write and Execute it without a separate dashboard.

## Lifecycle

1. Write the duration, for example 30 seconds; Execute resource 0.
2. Execute immediately returns **2.04 Changed** (request accepted). A two-second
   grace period allows the response to leave before WNM negotiation starts.
   CoAP retransmissions with the same peer/MID/token are acknowledged without
   scheduling another sleep, including buffered retries received after wake.
3. Stop IMU streaming and time notifications. Wait for outstanding Wakaama
   confirmable transactions and network TX to drain, for at most ten additional
   seconds. If they cannot drain, abort with `-EBUSY` instead of stranding a
   transaction.
4. Suspend Zephyr interface **TX** without bringing the interface down. This
   also prevents ARP/DHCP traffic from reaching the SDK during WNM sleep.
   Negotiate WNM through the SDK management path. AP rejection restores TX
   immediately; it does not start the sleep timer or fall back to disconnection.
5. Count the duration from successful WNM entry. No Wakaama steps or resource
   handling run while asleep, so server requests may timeout. Local short-button
   LED behavior continues; it does not wake the radio.
6. Exit WNM, always restore interface TX, request a registration update and
   resume native Observe processing, IMU subscriptions and the local clock.
   Preserve the Wakaama context and existing association on the normal path.

The AP must support WNM sleep (`wnm_sleep_mode=1`) and 802.11 power save must
remain enabled. HaLowLink2 was checked with WNM enabled and `dtim_period=10`;
no AP configuration change is required for this implementation.

Long sleeps can outlive DHCP leases, the 300-second LwM2M registration, or AP
association state. Native network/registration recovery may then be required.
A lost link during sleep causes an early WNM exit so ordinary recovery can run.
WNM exit may return an error even though the chip has left low power (for
example the AP no longer knows the station). The last result records that error.
No request is made to extend AP association/lease lifetimes silently.

SDK results include 1 unspecified error, 3 unavailable, 6 timed out and 13 not
supported. Local results include `-16` busy, `-19` no interface and `-100` link
down on Zephyr. Native host tests compare symbolic errno values because host
numbers can differ.

## Required SDK exit fix

The EdgeZ SDK patch clears the chip's WNM long-sleep flag after hardware
restoration on the powerdown exit path. During restoration the real SDK FSM
is still ACTIVE, so `UMAC_WNM_SLEEP_EVENT_HW_RESTARTED` re-enables that flag.
The previous powerdown exit path never cleared it. This allowed immediate
post-wake traffic while the chip was awake but suppressed later DTIM reception.
The corrected path clears it on both retained-chip and powerdown exits and
fetches the VIF ID again after interface restoration.

`python3 tests/test_wnm_sdk.py` compiles the exact pinned SDK source and its
real generated FSM against deterministic hardware stubs. The test reproduced
the retained long-sleep flag before the fix and passes afterward, including
changed VIF IDs and repeated cycles. Generated FSM files are not modified.

Powerdown wake calls `mmdrv_init()` to reload firmware/BCF, restore channel and
keys, then negotiate WNM exit; it is slower than ordinary DTIM idle wake. The
initial hardware run measured 1327 ms for that whole call, not just MCU wake.
It does not establish which phase dominates. The official MM8108 guide gives
an example powerdown WNM exit of 623 ms; neither figure is a guaranteed bound.

## Validation

Run `python3 tests/test_wnm_sleep.py` for the real object state machine and
platform wrapper against a fake AP and clock, under ASan/UBSan. It covers the
response grace period, duration limits, actual timer, pending transaction
protection, network TX drain, AP rejection, exit failure, link loss and repeated
requests. Build either `ez-cam01` or `ez-cam01-ble-prov` as usual.

A successful build does not prove WNM entry or reduced standby power. Verify
Execute response, timeout while asleep, post-wake Read/Write, cycle count and
last result on hardware; measure current with J-Link disconnected separately.

Reference: [Morse WNM Sleep API](https://morsemicro.github.io/mm-iot-sdk/framework/doc/html/group__MMWLAN__WNM.html).

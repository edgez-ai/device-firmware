# Connection recovery watchdog

The watchdog runs during ordinary DTIM operation as well as after WNM sleep.
It uses the native LwM2M registration/update exchange as its end-to-end check.
It does not reboot the MCU, erase credentials, or advertise BLE when recovery
is needed. This is network recovery, not a hardware CPU-lockup watchdog.

## Normal operation

- After a successful registration/update ACK, allow 60 seconds before asking
  Wakaama for another registration update. Native updates also refresh this
  deadline, so no second probe is needed immediately after one succeeds.
- Allow 45 seconds for the response. On failure, rebuild the client context and
  register again. A native registration rejection/failure triggers this first
  recovery immediately rather than remaining in an unusable bootstrap state.
- If the fresh registration attempt also fails, request HaLow disconnect /
  reconnect on the system workqueue. DHCP and registration run again normally.
- Repeated Wi-Fi reconnect requests back off from 120 to 240 to at most 300
  seconds. Driver-managed association retries continue independently. A
  successful registration resets this backoff. No endless MCU reboot cycle is
  introduced if an AP or server remains unavailable.
- Ordinary driver link-loss events still recover directly; the watchdog is a
  fallback for a stale association, missing registration response or failed
  native client state. It never infers health just from a local IP address.

`CONFIG_EZ_DEV54_CONNECTION_WATCHDOG` defaults on. Build settings
`CONFIG_EZ_DEV54_CONNECTION_PROBE_SECONDS` (60) and
`CONFIG_EZ_DEV54_CONNECTION_RESPONSE_SECONDS` (45) control the two deadlines.
Probes add registration traffic during normal operation and can affect average
energy use. They do not change DTIM10 or the UART/logging configuration.

## WNM and long sleep

WNM sleep duration remains 1–86400 seconds. This is the firmware limit, not a
promise that an AP association will survive for a day. AP restart, policy,
rekeying behavior and lease limits can require a new association after wake.
The current gateway DHCP lease setting was checked as 12 hours. The firmware's
initial LwM2M registration lifetime is 300 seconds.

The watchdog is paused throughout WNM. It schedules no keepalive probes and no
watchdog timer wakes during that interval. DHCP/IP expiration does not shorten
the requested sleep; connection recovery waits until its timer finishes.

On wake, an expired native registration is replaced with a new registration,
rather than updating an expired location. A still-valid registration must pass
a fresh update/ACK check. If Wi-Fi/IP is unavailable, reconnect it. The normal
45-second recovery and backoff policy then applies. Endpoint identity, factory
data and saved Wi-Fi credentials are retained.

After a full registration/context rebuild, old Observe subscriptions may be
lost and the server/UI must subscribe again. Short WNM sleep on a healthy,
unexpired session keeps the existing context and native subscriptions.

## Diagnostics

Vendor object **31027**, instance 0, is Read-only and available in the generic
Wakaama UI using the matching XML model. It is not an OMA-registered object.

| Resource | Meaning |
| --- | --- |
| /31027/0/0 | State: 0 offline, 1 registering, 2 online, 3 probing, 4 registering again, 5 reconnecting, 6 asleep |
| /31027/0/1 | Probe interval, seconds |
| /31027/0/2 | Response deadline, seconds |
| /31027/0/3 | Last successful registration ACK age, seconds; -1 if none |
| /31027/0/4 | Watchdog LwM2M restart request count |
| /31027/0/5 | Watchdog Wi-Fi reconnect request count |
| /31027/0/6 | Last reason: 0 none, 1 expired, 2 timeout, 3 native failure, 4 unavailable link |
| /31027/0/7 | Most recently observed effective registration lifetime, seconds |

Counters are since boot and do not count ordinary driver-driven reassociation.
The last reason remains readable after recovery. Read does not send a probe.

## Teardown safety

The pinned Wakaama close fix runs pending transaction cancellation callbacks
before freeing native server/Observe lists. The previous ordering produced an
ASan-confirmed use-after-free even on closing a registered client, including
its newly queued deregistration callback. Safe teardown is necessary for
repeated watchdog and ordinary AP-restart recovery.

## Validation

`python3 tests/test_connection_watchdog.py` covers successful ACKs, silent AP /
server failure, staged recovery, bounded backoff, rejected registrations,
expired long sleep, short sleep requiring a fresh ACK, and ordinary link loss.
`python3 tests/test_wakaama_close.py` compiles the full actual Wakaama client
core with ASan/UBSan and exercises registered/pending context cleanup.
`python3 tests/test_wnm_sleep.py` also verifies lease/link loss does not wake the
radio before the configured sleep timer.

Build success and simulated tests do not establish AP-specific maximum sleep
or the watchdog image's physical power floor. Record hardware results in
[VALIDATION.md](VALIDATION.md).

# Initial camera migration

Source: `edgez-ai/openwrt-firmware`, commit
`cb9130aa56edd114ebb9330041d12c154c9c5b24`, directory
`tests/nrf54l15-presence-mqtt/nrf54`.

The import uses the committed 3 mW camera baseline, not uncommitted radar
rail benchmarks. Application C/header files and board/config files are
copied from that revision. Retained compatibility helpers carry historical
radar names; their acquisition/CLI paths are disabled in the camera build.

Integration changes:

- Project-local `firmware/` layout and camera-only PlatformIO environments.
- Direct, pinned EdgeZ Morse and Wakaama client-core submodules.
- Wakaama lookup changed from sibling `wakaama-server`/OpenWrt directories
  to `third_party/wakaama` within this repository.
- Standalone build help and a J-Link uploader with no automatic recovery.
- Credentials remain local Git-ignored files; examples contain placeholders.

The startup UART standby helper, camera capture/cache code, endpoint/server
discovery, CoAP block handling, power-save settings, board wiring and
button/light behavior are intentionally retained. No new night schedule,
deep-sleep mode or power optimization is implemented during this migration.

Build validation and hardware validation must be distinguished: the inherited
source was measured at approximately 3 mW on the original carrier. This import
does not itself establish new flash, photo or current measurements. See
`VALIDATION.md` for the completed migration checks.

# Third-party and component license notices

EZ-DEV54's root [LICENSE](LICENSE) contains Apache-2.0. Dependencies and
separately licensed components remain governed by their own license terms,
copyright notices and file-level SPDX identifiers. This notice identifies
the following components; it does not relicense them or replace their terms.
Listing a component here does not mean every firmware profile links it.

## EdgeZ HaLow Mesh

The EdgeZ-owned portions of `edgez-halow-mesh`, including its portable core
and public headers when distributed as a prebuilt library, are governed by
the **EdgeZ Non-Commercial and Commercial Software License, Version 1.0**.
Copyright (c) 2026 EdgeZeron AI AB. All rights reserved.

The license permits the non-commercial purposes and internal evaluation
specified in its terms. **Commercial use requires a separate written
commercial license from EdgeZeron AI AB.** Third-party code included with
the mesh component retains its own licenses and notices.

- Complete license: [licenses/EDGEZERON-NON-COMMERCIAL.txt](licenses/EDGEZERON-NON-COMMERCIAL.txt).
- Upstream: [edgez-ai/edgez-halow-mesh](https://github.com/edgez-ai/edgez-halow-mesh).
- Prebuilt component scope and attributions: [static library package notice](https://github.com/edgez-ai/edgez-halow-mesh/blob/main/packaging/STATIC-LIBRARY-NOTICE.md).

The license copy above is reproduced unchanged from the mesh repository's
`LICENSE` file.

## Morse Micro MM-IoT-SDK

The SDK is located at
`firmware/platforms/mm-iot-zephyr/submodules/mm-iot-sdk`.
Its precompiled hosted-driver libraries and other binaries identified as
covered by **LicenseRef-MorseMicroBDL** are governed by the **Morse Micro
Binary Distribution License Agreement**, with rights retained by Morse
Micro Pty. Ltd. and its licensors.

The agreement specifies use with hardware containing Morse Micro Wi-Fi
HaLow chips and conditions for binary redistribution. Distribute covered
material with the applicable agreement and retain its copyright notices.
Refer to the complete agreement for the permissions and restrictions.

- Complete binary license: [licenses/LicenseRef-MorseMicroBDL.txt](licenses/LicenseRef-MorseMicroBDL.txt), copied unchanged from the SDK's `LICENSES/LicenseRef-MorseMicroBDL.txt`.
- SDK license overview: [MM-IoT-SDK README](firmware/platforms/mm-iot-zephyr/submodules/mm-iot-sdk/README.md).
- Hosted-library license scope: [morselib README](firmware/platforms/mm-iot-zephyr/submodules/mm-iot-sdk/framework/morselib/README.md).
- Complete SDK license directory: [LICENSES](firmware/platforms/mm-iot-zephyr/submodules/mm-iot-sdk/LICENSES).

**The binary license is not a blanket license for all SDK source.** The SDK
overview specifies Apache-2.0 unless individual files state otherwise. In
particular, the hosted-library source is offered under GPL-3.0-or-later or a
separate Morse Micro commercial license, as described in its README and
file-level notices. Other bundled third-party components retain their own
licenses. The separately pinned `morse-firmware` dependency also provides
vendor-specific licenses in its firmware and board-configuration directories.

## Other dependencies

Other dependency notices remain applicable, including the Apache-2.0
MM-IoT Zephyr integration and the Wakaama license files and
[NOTICE](third_party/wakaama/NOTICE.md). Preserve the applicable notices when
distributing source, libraries or firmware containing those components.

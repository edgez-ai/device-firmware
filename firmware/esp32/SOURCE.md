# Source provenance

ESP32 application code imported from `edgez-ai/template-offgrid-tracker`
commit `0a54dbe`; original Apache-2.0 notices and author history remain available
in that repository. The shared generated NetworkPacket schema was extracted
from its common protocol definitions. nRF54 application/Zephyr sources were
not relocated. RF/SDK code remains in pinned submodules under its own licenses.

The ESP platform tracks the `edgez` branch and is pinned at `f6477c2116e1f00c9766ba1afb64485d05aada26`.
The consumer-owned `patches/esp-halow-station-adapter.patch` adds the station
adapter to its CMake source list. Runtime country selection and reset-safe
GPIO initialization are supplied by the application component.

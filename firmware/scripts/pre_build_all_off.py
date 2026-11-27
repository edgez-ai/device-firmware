"""Keep the isolated all-off image independent of Morse RF build targets."""
Import("env")
import os
for name in ("RADAR_PIO_PREBUILD_TARGET", "MORSE_ZEPHYR_MODULE"):
    env["ENV"].pop(name, None)
    os.environ.pop(name, None)

#!/usr/bin/env python3
"""Prepare the PlatformIO build of the nRF54L15 radar host firmware.

1. Locate the official Morse Micro Zephyr module (git submodule
   platforms/mm-iot-zephyr, or MORSE_ZEPHYR_MODULE) and hand it to CMake.
2. Make PlatformIO's Zephyr builder run the module's build-time custom
   commands. PlatformIO compiles from CMake's code model with SCons and does
   not execute custom targets, but the module creates its merged libmorse
   archive and the firmware/BCF include files that way. zephyr/CMakeLists.txt
   collects them in the target "radar_pio_prebuild", which is built with
   ninja before SCons compiles anything.

The builder patches follow template-offgrid-tracker's
firmware/scripts/pre_build_firmware_nrf54.py.
"""
Import("env")  # noqa: F821  (provided by PlatformIO)
import os
from pathlib import Path

PROJECT_DIR = Path(env.subst("$PROJECT_DIR")).resolve()  # noqa: F821
PREBUILD_TARGET = "radar_pio_prebuild"
SUBMODULE_HELP = (
    "  git submodule update --init --recursive -- "
    "firmware/platforms/mm-iot-zephyr"
)


def fail(message):
    print("ERROR: " + message)
    env.Exit(1)  # noqa: F821


def find_morse_module():
    override = os.environ.get("MORSE_ZEPHYR_MODULE", "").strip()
    root = Path(override).expanduser() if override else PROJECT_DIR / "platforms" / "mm-iot-zephyr"
    root = root.resolve()
    if not (root / "zephyr" / "module.yml").is_file():
        fail("Morse Micro Zephyr module not found at %s. From the repository root run:\n%s"
             % (root, SUBMODULE_HELP))
    sdk = root / "submodules" / "mm-iot-sdk" / "framework" / "morselib"
    firmware = root / "submodules" / "morse-firmware" / "bcf"
    if not sdk.is_dir() or not firmware.is_dir():
        fail("The Morse Micro module's own submodules are missing. Run:\n"
             "  git -C %s submodule update --init --recursive" % root)
    return root


def patch_platformio_builder():
    """Teach the Zephyr builder to build a ninja target before compiling."""
    framework = env.PioPlatform().get_package_dir("framework-zephyr-nrf54l15")  # noqa: F821
    if not framework:
        return
    builder = Path(framework) / "scripts" / "platformio" / "platformio-build.py"
    if not builder.is_file():
        fail("Unsupported Zephyr package layout: %s is missing" % builder)

    contents = builder.read_text(encoding="utf-8")
    changed = False

    # A single library path must be appended as a list element.
    old = 'env.Append(LIBS=lib_dep["lib_path"])'
    if old in contents:
        contents = contents.replace(old, 'env.Append(LIBS=[lib_dep["lib_path"]])')
        changed = True

    hook = (
        "\n# Added by iwrl6432-radar-firmware/nrf54/scripts/pre_build_nrf54.py:\n"
        "# build custom-command outputs that SCons cannot produce, then reload the\n"
        "# code model in case ninja regenerated CMake.\n"
        'if os.environ.get("RADAR_PIO_PREBUILD_TARGET"):\n'
        '    ninja = os.path.join(platform.get_package_dir("tool-ninja"), "ninja")\n'
        "    subprocess.check_call(\n"
        '        [ninja, "-C", BUILD_DIR, os.environ["RADAR_PIO_PREBUILD_TARGET"]])\n'
        "    codemodel = get_cmake_code_model(west_manifest)\n"
        "    target_configs = load_target_configurations(codemodel)\n"
    )
    if hook not in contents:
        anchor = "target_configs = load_target_configurations(codemodel)\n"
        position = contents.find("\n" + anchor)
        if position < 0:
            fail("Unsupported Zephyr PlatformIO builder: cannot add the prebuild step")
        position += 1 + len(anchor)
        contents = contents[:position] + hook + contents[position:]
        changed = True

    if changed:
        builder.write_text(contents, encoding="utf-8")
        print("Patched the Zephyr PlatformIO builder for the Morse Micro module")


module_root = find_morse_module()
patch_platformio_builder()

for key, value in (
    ("MORSE_ZEPHYR_MODULE", str(module_root)),
    ("RADAR_PIO_PREBUILD_TARGET", PREBUILD_TARGET),
):
    env["ENV"][key] = value  # noqa: F821
    os.environ[key] = value

print("Morse Micro Zephyr module: %s" % module_root)
print("EZ-CAM01: VC0706 camera over LwM2M/HaLow")

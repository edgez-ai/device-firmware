#!/usr/bin/env python3
"""Program and verify EZ-CAM01 over J-Link/SWD, then reset and run."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="Built firmware.hex")
    parser.add_argument("--serial", default=os.environ.get("JLINK_SERIAL"))
    parser.add_argument("--speed", type=int, default=4000, help="SWD speed in kHz")
    parser.add_argument("--jlink", default=os.environ.get("JLINK_EXE"))
    args = parser.parse_args()
    image = args.image.resolve()
    if not image.is_file() or image.suffix.lower() != ".hex":
        parser.error("Provide an existing firmware.hex")
    if any(c in str(image) for c in '\r\n"'):
        parser.error("Unsupported character in image path")
    if args.speed <= 0 or (args.serial and not args.serial.isdigit()):
        parser.error("Use a positive SWD speed and a numeric probe serial")
    executable = args.jlink or shutil.which("JLinkExe") or shutil.which("JLink.exe")
    if executable is None and Path("/usr/local/bin/JLinkExe").is_file():
        executable = "/usr/local/bin/JLinkExe"
    if executable is None:
        parser.error("Install SEGGER J-Link or set JLINK_EXE")
    with tempfile.TemporaryDirectory(prefix="ez-cam01-jlink-") as folder:
        script = Path(folder) / "flash.jlink"
        script.write_text(f'r\nloadfile "{image}"\nr\ng\nexit\n')
        command = [str(executable), "-device", "NRF54L15_M33", "-if", "SWD",
                   "-speed", str(args.speed), "-AutoConnect", "1", "-ExitOnError", "1"]
        if args.serial:
            command += ["-SelectEmuBySN", args.serial]
        command += ["-CommandFile", str(script)]
        try:
            return subprocess.call(command)
        except OSError as error:
            parser.exit(1, f"J-Link launch failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())

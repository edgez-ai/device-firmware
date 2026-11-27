# SPDX-License-Identifier: Apache-2.0
import os
import subprocess
from pathlib import Path
Import("env")
if env.subst("$PIOENV") == "ez-dev01":
    build_dir = Path(env.subst("$BUILD_DIR"))
    ninja = Path(env.PioPlatform().get_package_dir("tool-ninja")) / "ninja"
    subprocess.run([str(ninja), "-C", str(build_dir), "morselib_build", "companion_crypto_archives"], check=True, env=env["ENV"])
    # CMake calls this generated archive internal, but it has no static-library
    # target for PlatformIO to discover. Keep it explicitly in SCons' link.
    env.Append(LIBS=[env.File(str(build_dir / 'esp-idf/halow/components/morselib/libmorse.a'))])
    env["LIBS"] = [lib for lib in env.get("LIBS", [])
                   if str(lib).split("/")[-1] not in
                   ("libedgez_halow_mesh_platform.a", "edgez_halow_mesh_platform", "-ledgez_halow_mesh_platform")]
    env.Append(LIBS=[env.File(str(build_dir / "esp-idf/main/companion_crypto/libedgez_halow_mesh_platform.a"))])

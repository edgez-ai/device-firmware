import os
from pathlib import Path

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
pio_env = env.subst("$PIOENV")
target = env.BoardConfig().get("build.mcu", "")
target_defaults = project_dir / f"sdkconfig.defaults.{target}"
environment_defaults = project_dir / f"sdkconfig.defaults.{pio_env}"

defaults = [project_dir / "sdkconfig.defaults"]
if target_defaults.is_file():
    defaults.append(target_defaults)
if environment_defaults.is_file():
    defaults.append(environment_defaults)

env["ENV"]["EZ_DEV01_BUILD"] = "1" if pio_env == "ez-dev01" else "0"

env["ENV"]["SDKCONFIG_DEFAULTS"] = ";".join(str(path) for path in defaults)

# This pinned platform omits the non-mesh station adapter from its source list.
# Apply only the reviewed consumer patch; reject unrelated platform revisions.
if pio_env == "ez-dev01":
    import subprocess
    platform_dir = project_dir / "platforms/esp-halow"
    patch = project_dir / "patches/esp-halow-station-adapter.patch"
    check = ["git", "-C", str(platform_dir), "apply", "--check", str(patch)]
    if subprocess.run(check, capture_output=True).returncode == 0:
        subprocess.run(["git", "-C", str(platform_dir), "apply", str(patch)], check=True)
    else:
        subprocess.run(["git", "-C", str(platform_dir), "apply", "--reverse", "--check", str(patch)], check=True)

# PlatformIO checks only top-level CMakeLists files. Refresh its code model
# before compiling when the consumer's included target configuration changes.
if pio_env == "ez-dev01":
    cmake_cache = Path(env.subst("$BUILD_DIR")) / "CMakeCache.txt"
    consumer_inputs = [project_dir / "main/dev01.cmake", project_dir / "main/companion_mesh.cmake"]
    if cmake_cache.is_file() and any(path.stat().st_mtime > cmake_cache.stat().st_mtime for path in consumer_inputs):
        cmake_cache.unlink()

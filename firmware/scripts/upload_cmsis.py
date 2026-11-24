"""PlatformIO upload using the carrier's CMSIS-DAP probe."""
Import("env")
env.Replace(UPLOADCMD='"$PYTHONEXE" "$PROJECT_DIR/scripts/nrf54_openocd.py" flash "$BUILD_DIR/firmware.hex"')

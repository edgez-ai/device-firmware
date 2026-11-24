"""Use J-Link for PlatformIO uploads without an automatic mass erase."""
Import("env")
env.Replace(UPLOADCMD='"$PYTHONEXE" "$PROJECT_DIR/scripts/flash_jlink.py" "$BUILD_DIR/firmware.hex"')

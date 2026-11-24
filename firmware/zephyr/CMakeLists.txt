# EZ-CAM01 nRF54L15 + FGH200M camera firmware.
# Build from firmware/ with PlatformIO; platformio.ini supplies the camera flags.

cmake_minimum_required(VERSION 3.20.0)

set(RADAR_NRF54_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(RADAR_REPO_DIR "${RADAR_NRF54_DIR}/..")

if(NOT DEFINED DTC_OVERLAY_FILE)
  set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/boards/edgez_nrf54l15_fgh200m_radar.overlay")
endif()

# Optional, untracked settings such as the HaLow SSID and password.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.conf")
  list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/secrets.conf")
endif()

# Apply the comparison profile last so private settings cannot enable MQTT.
if(RADAR_DISABLE_MQTT)
  list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/presence_no_mqtt.conf")
endif()

if(RADAR_HALOW_LIGHT)
  list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/halow_light.conf")
  if(RADAR_HALOW_COAP_UDP OR RADAR_HALOW_CAMERA)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/halow_coap_udp.conf")
  endif()
  if(RADAR_HALOW_CAMERA)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/halow_camera.conf")
  endif()
  if(RADAR_HALOW_LIGHT_POWER)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/halow_light_power.conf")
  endif()
  if(RADAR_HALOW_LIGHT_LISTEN)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/halow_light_listen.conf")
  endif()
  if(RADAR_HALOW_NO_LOG)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/halow_no_log.conf")
  endif()
  if(RADAR_HALOW_IRQ_NO_PULL)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/halow_irq_no_pull.conf")
  endif()
  if(RADAR_HALOW_BUSY_WAKE)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/halow_busy_wake.conf")
  endif()
  if(RADAR_HALOW_DIAG)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/halow_diag.conf")
  endif()
endif()

# Apply gateway-specific settings after the shared private configuration.
if(RADAR_HALOWLINK2)
  list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/halowlink2.conf")
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/halowlink2-secrets.conf")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/halowlink2-secrets.conf")
  elseif(NOT EZ_CAM_BLE_PROV)
    message(FATAL_ERROR "Create zephyr/halowlink2-secrets.conf with CONFIG_RADAR_HALOW_PASSWORD for this AP")
  endif()
endif()

if(RADAR_HALOW_SLEEP_DIAG)
  list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/halow_sleep_diag.conf")
endif()

if(EZ_CAM_BLE_PROV)
  list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/ble_prov.conf")
  list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/factory_data.overlay"
    "${CMAKE_CURRENT_LIST_DIR}/camera_uart_deferred.overlay")
endif()

if(EZ_CAM_SENSORS)
  list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/sensors_on_demand.conf")
endif()

# Pinned EdgeZ Morse Micro Zephyr module. PlatformIO does not use a west
# workspace, so the module is the git submodule firmware/platforms/mm-iot-zephyr
# (or the directory named by MORSE_ZEPHYR_MODULE). A west workspace created
# from the module's own manifest already contains it.
if(DEFINED ENV{MORSE_ZEPHYR_MODULE})
  list(APPEND ZEPHYR_EXTRA_MODULES "$ENV{MORSE_ZEPHYR_MODULE}")
elseif(EXISTS "${RADAR_NRF54_DIR}/platforms/mm-iot-zephyr/zephyr/module.yml")
  list(APPEND ZEPHYR_EXTRA_MODULES "${RADAR_NRF54_DIR}/platforms/mm-iot-zephyr")
endif()

# Some packaged frameworks omit executable bits on Python build tools.
if(DEFINED ENV{ZEPHYR_BASE} AND EXISTS "$ENV{ZEPHYR_BASE}/scripts/build/gen_kobject_list.py")
  set(GEN_KOBJECT_LIST "$ENV{ZEPHYR_BASE}/scripts/build/gen_kobject_list.py" CACHE FILEPATH "Zephyr kobject generator" FORCE)
endif()
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ez_cam01)

if(NOT CONFIG_WIFI_MORSEMICRO)
  message(FATAL_ERROR
    "The Morse Micro Zephyr module was not found. Run\n"
    "  git submodule update --init --recursive -- firmware/platforms/mm-iot-zephyr\n"
    "or set MORSE_ZEPHYR_MODULE to a checkout of "
    "https://github.com/edgez-ai/mm-iot-zephyr (with its submodules).")
endif()

# ---- Application ------------------------------------------------------------

target_include_directories(app PRIVATE
  "${RADAR_NRF54_DIR}/src"
  "${RADAR_NRF54_DIR}/lib/iwrl6432"
)
target_sources(app PRIVATE
  "${RADAR_NRF54_DIR}/src/main.c"
  "${RADAR_NRF54_DIR}/src/radar_app.c"
  "${RADAR_NRF54_DIR}/src/halow_sta.c"
  "${RADAR_NRF54_DIR}/src/radar_mqtt.c"
)
if(NOT CONFIG_RADAR_SIMULATE_PRESENCE)
  target_sources(app PRIVATE
    "${RADAR_NRF54_DIR}/src/radar_link.c"
    "${RADAR_NRF54_DIR}/lib/iwrl6432/iwrl6432_stream.c"
  )
endif()

if(CONFIG_RADAR_CONFIGURE_SENSOR)
  set(radar_profile "${RADAR_NRF54_DIR}/config/human_detect_lowpower_10s_xwrl6432.cfg")
  set(radar_embed "${RADAR_NRF54_DIR}/scripts/embed_blob.py")
  set(radar_config_dir "${CMAKE_BINARY_DIR}/radar_blobs")
  # PlatformIO consumes the generated sources without running build-time CMake commands.
  execute_process(
    COMMAND "${PYTHON_EXECUTABLE}" "${radar_embed}" --symbol radar_config
            --input "${radar_profile}" --output "${radar_config_dir}/radar_config.inc"
    COMMAND_ERROR_IS_FATAL ANY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${radar_profile}" "${radar_embed}")
  target_include_directories(app PRIVATE "${radar_config_dir}")
  target_sources(app PRIVATE "${RADAR_NRF54_DIR}/lib/iwrl6432/iwrl6432_cli.c")
endif()

# mmwlan.h, for the HaLow power-save calls.
target_link_libraries(app PRIVATE libmorse_interface)

# GNU Arm Embedded newlib does not provide the Linux-style endian.h used
# by Morse hostap. Provide the Cortex-M little-endian compatibility header.
foreach(morse_target mmhostap morselib mmutils)
  if(TARGET ${morse_target})
    target_include_directories(${morse_target} PRIVATE "${RADAR_NRF54_DIR}/compat")
  endif()
endforeach()

# PlatformIO compiles from CMake's code model and does not run the module's
# custom commands (merged and mangled libmorse archive, firmware and BCF
# includes). scripts/pre_build_nrf54.py makes PlatformIO build this target with
# ninja before it compiles anything.
add_custom_target(radar_pio_prebuild)
add_dependencies(radar_pio_prebuild libmorse_build firmware)

if(RADAR_HALOW_LIGHT)
  set(WAKAAMA_CORE "${RADAR_NRF54_DIR}/../third_party/wakaama" CACHE PATH "Pinned Wakaama client core")
  if(NOT EXISTS "${WAKAAMA_CORE}/core/liblwm2m.c")
    message(FATAL_ERROR "Initialize third_party/wakaama with git submodule update --init --recursive")
  endif()
  set(client_examples "${WAKAAMA_CORE}/examples/client/common")
  file(GLOB wakaama_protocol_sources CONFIGURE_DEPENDS
    "${WAKAAMA_CORE}/core/*.c" "${WAKAAMA_CORE}/data/*.c"
    "${WAKAAMA_CORE}/coap/*.c" "${WAKAAMA_CORE}/coap/er-coap-13/*.c")
  zephyr_library_named(halow_wakaama)
  zephyr_library_sources("${client_examples}/object_security.c"
    "${client_examples}/object_server.c" "${client_examples}/object_device.c"
    ${wakaama_protocol_sources})
  target_include_directories(halow_wakaama PUBLIC "${WAKAAMA_CORE}/include"
    PRIVATE "${WAKAAMA_CORE}/core" "${WAKAAMA_CORE}/coap")
  target_compile_definitions(halow_wakaama PUBLIC
    LWM2M_CLIENT_MODE LWM2M_LITTLE_ENDIAN
    LWM2M_SUPPORT_TLV
    LWM2M_COAP_DEFAULT_BLOCK_SIZE=256
    LWM2M_COAP_MAX_MESSAGE_SIZE=1024 LWM2M_COAP_DEFAULT_MAX_RETRANSMIT=4
    LWM2M_COAP_SEPARATE_TIMEOUT=15)
  if(NOT RADAR_HALOW_CAMERA)
    target_compile_definitions(halow_wakaama PUBLIC LWM2M_SUPPORT_SENML_JSON)
  endif()
  target_compile_definitions(halow_wakaama PRIVATE _POSIX_C_SOURCE=200809L)
  target_link_libraries(app PRIVATE halow_wakaama)
  target_compile_definitions(app PRIVATE RADAR_HALOW_LIGHT)
  target_sources(app PRIVATE "${RADAR_NRF54_DIR}/src/halow_light.c")
  if(CONFIG_RADAR_HALOW_SLEEP_DIAG)
    target_sources(app PRIVATE "${RADAR_NRF54_DIR}/src/halow_sleep_diag.c")
  endif()
  if(RADAR_HALOW_COAP_UDP OR RADAR_HALOW_CAMERA)
    target_compile_definitions(app PRIVATE RADAR_HALOW_COAP_UDP)
  endif()
  if(RADAR_HALOW_CAMERA)
    target_compile_definitions(app PRIVATE RADAR_HALOW_CAMERA)
    target_sources(app PRIVATE "${RADAR_NRF54_DIR}/src/halow_camera.c")
    target_link_options(zephyr_interface INTERFACE -Wl,--wrap=lwm2m_data_serialize)
  endif()
endif()

if(CONFIG_EZ_CAM_BLE_PROV)
  target_sources(app PRIVATE "${RADAR_NRF54_DIR}/src/ble_prov.c"
    "${RADAR_NRF54_DIR}/src/prov_protocol.c"
    "${RADAR_NRF54_DIR}/src/factory_record.c"
    "${RADAR_NRF54_DIR}/src/factory_data.c")
endif()

if(CONFIG_EZ_CAM_SENSORS)
  target_sources(app PRIVATE "${RADAR_NRF54_DIR}/src/sensors_hw.c"
    "${RADAR_NRF54_DIR}/src/halow_sensors.c"
    "${RADAR_NRF54_DIR}/src/halow_device.c"
    "${RADAR_NRF54_DIR}/src/device_platform.c"
    "${RADAR_NRF54_DIR}/src/halow_time_sync.c")
  target_include_directories(app PRIVATE "${WAKAAMA_CORE}/coap")
  execute_process(COMMAND git -C "${RADAR_REPO_DIR}" describe --always --dirty
    OUTPUT_VARIABLE ez_cam_revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT ez_cam_revision)
    set(ez_cam_revision "unversioned")
  endif()
  target_compile_definitions(app PRIVATE EZ_CAM_APP_REV="${ez_cam_revision}")
endif()

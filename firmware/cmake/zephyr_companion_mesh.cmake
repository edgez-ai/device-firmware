# Keep the public station API and the private mesh RF driver in one image.
# Morse's station archive already namespaces its driver internals. The mesh
# driver remains a separate RF-only archive and is activated only while the
# unprovisioned BLE companion owns the radio.
function(ez_add_zephyr_companion_mesh)
  if(DEFINED ENV{MORSE_ZEPHYR_MODULE})
    set(MORSE_ZEPHYR_MODULE "$ENV{MORSE_ZEPHYR_MODULE}")
  else()
    set(MORSE_ZEPHYR_MODULE "${RADAR_NRF54_DIR}/platforms/mm-iot-zephyr")
  endif()
  set(ZEPHYR_CURRENT_MODULE_DIR "${MORSE_ZEPHYR_MODULE}")
  set(MMIOT_ROOT "${MORSE_ZEPHYR_MODULE}/submodules/mm-iot-sdk")
  set(CMAKE_CURRENT_SOURCE_DIR "${MORSE_ZEPHYR_MODULE}/components/morsemicro/libmorse")
  get_target_property(COMPILE_OPTIONS morselib COMPILE_OPTIONS)
  if(NOT COMPILE_OPTIONS)
    set(COMPILE_OPTIONS "")
  endif()
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/edgez_join_only.cmake" mesh_driver)
  # Reuse the reviewed RF-only source boundary without replacing libmorse.
  string(REPLACE "libmorse_build" "ez_companion_morse_rf_build" mesh_driver "${mesh_driver}")
  string(REGEX REPLACE "(^|[^A-Za-z0-9_])libmorse([^A-Za-z0-9_]|$)" "\\1ez_companion_morse_rf\\2" mesh_driver "${mesh_driver}")
  string(REPLACE "OUTPUT_NAME morse" "OUTPUT_NAME ez_companion_morse_rf" mesh_driver "${mesh_driver}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ez_companion_mesh_driver.cmake" "${mesh_driver}")
  include("${CMAKE_CURRENT_BINARY_DIR}/ez_companion_mesh_driver.cmake")
  target_link_libraries(ez_companion_morse_rf PRIVATE zephyr_interface)
  if(CONFIG_DT_HAS_MORSEMICRO_MM8108_ENABLED)
    set(companion_chip mm8108)
  else()
    set(companion_chip mm6108)
  endif()
  # Each driver's HAL selection must return its own operation table.
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/companion_chip.c"
    "#include \"mmhal_wlan.h\"\nconst struct mmhal_chip *mmhal_get_chip(void) { return &mmhal_${companion_chip}; }\n")
  target_sources(ez_companion_morse_rf PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/companion_chip.c")
  target_compile_definitions(ez_companion_morse_rf PRIVATE _DEFAULT_SOURCE=1
    mmhal_get_chip=ez_companion_get_chip
    mmhal_mm6108=ez_companion_mmhal_mm6108
    mmhal_mm8108=ez_companion_mmhal_mm8108
    morse_xtal_init_delay=ez_companion_xtal_init_delay
    mmhal_spi_irq_poll_interval=ez_companion_spi_irq_poll_interval)
  target_compile_options(ez_companion_morse_rf PRIVATE -include strings.h)
  target_link_libraries(edgez_rf_morsemicro PRIVATE zephyr_interface)
  target_include_directories(app PRIVATE
    "${EDGEZ_MESH_INCLUDE_ROOT}"
    "${EDGEZ_RF_API_ROOT}/include"
    "${EDGEZ_RF_MORSEMICRO_ROOT}/include")
  target_sources(edgez_rf_morsemicro PRIVATE
    "${EDGEZ_RF_MORSEMICRO_ROOT}/src/edgez_morse_channel.c")
  target_include_directories(edgez_rf_morsemicro PRIVATE "${MMIOT_ROOT}/framework/src/mmregdb")
  get_target_property(platform_archive edgez_mesh_platform IMPORTED_LOCATION)
  set(crypto_output "${CMAKE_CURRENT_BINARY_DIR}/companion_crypto")
  get_filename_component(platform_name "${platform_archive}" NAME)
  get_filename_component(security_name "${EDGEZ_PREBUILT_SECURITY_LIBRARY}" NAME)
  set(namespace_script "${RADAR_NRF54_DIR}/scripts/namespace_companion_crypto.py")
  add_custom_command(OUTPUT "${crypto_output}/${platform_name}" "${crypto_output}/${security_name}"
    COMMAND "${Python3_EXECUTABLE}" "${namespace_script}" --nm "${CMAKE_NM}"
      --objcopy "${CMAKE_OBJCOPY}" --output "${crypto_output}"
      "${platform_archive}" "${EDGEZ_PREBUILT_SECURITY_LIBRARY}"
    DEPENDS "${namespace_script}" "${platform_archive}" "${EDGEZ_PREBUILT_SECURITY_LIBRARY}")
  add_custom_target(companion_crypto_archives DEPENDS
    "${crypto_output}/${platform_name}" "${crypto_output}/${security_name}")
  set_target_properties(edgez_mesh_platform PROPERTIES IMPORTED_LOCATION "${crypto_output}/${platform_name}")
  set_target_properties(edgez_mesh_security PROPERTIES IMPORTED_LOCATION "${crypto_output}/${security_name}")
  add_dependencies(edgez_mesh_platform companion_crypto_archives)
  add_dependencies(edgez_mesh_security companion_crypto_archives)
  set(EDGEZ_PREBUILT_SECURITY_LIBRARY "${crypto_output}/${security_name}")
  target_link_libraries(app PRIVATE "-Wl,--start-group"
    "$<TARGET_FILE:edgez_mesh_platform>" "${EDGEZ_PREBUILT_SECURITY_LIBRARY}"
    "$<TARGET_FILE:edgez_mesh_security_glue>" "$<TARGET_FILE:edgez_umac_core>"
    "$<TARGET_FILE:edgez_rf_morsemicro>" "$<TARGET_FILE:ez_companion_morse_rf>"
    "-Wl,--end-group")
  target_link_libraries(app PRIVATE edgez_umac_core edgez_mesh_platform
    edgez_mesh_security edgez_mesh_security_glue edgez_rf_morsemicro ez_companion_morse_rf)
  target_sources(app PRIVATE
    "${RADAR_NRF54_DIR}/src/companion_mesh_protocol.c"
    "${RADAR_NRF54_DIR}/src/companion_mesh_radio.c"
    "${RADAR_NRF54_DIR}/src/companion_mesh_zephyr.c")
endfunction()
ez_add_zephyr_companion_mesh()

if(TARGET radar_pio_prebuild)
  add_dependencies(radar_pio_prebuild ez_companion_morse_rf edgez_rf_morsemicro companion_crypto_archives)
endif()

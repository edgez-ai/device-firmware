# Normal station and companion mesh share the HAL, with separate driver state.
set(mesh_halow "${CMAKE_CURRENT_LIST_DIR}/../platforms/esp-halow/halow")
function(add_companion_radio)
 set(MM_IOT_SDK "${mesh_halow}/components/mm-iot-sdk")
 set(EDGEZ_HALOW_MESH_PREBUILT_ROOT "${mesh_halow}/components/edgez-halow-mesh-prebuilt")
 set(MM_IOT_EDGEZ_INCLUDE_DIRS "${EDGEZ_HALOW_MESH_PREBUILT_ROOT}/include")
 include("${mesh_halow}/components/edgez-rf-api/component.cmake")
 include("${mesh_halow}/components/edgez-rf-morsemicro/component.cmake")
 set(EDGEZ_RF_ONLY_MODE ON)
 # Reuse the SDK's explicit RF-only source list without changing its station build.
 include("${mesh_halow}/components/morselib/CMakeLists.txt")
 # PlatformIO keys objects by source path. Give the RF compilation its own
 # translation units so the station and RF flags cannot share an object.
 get_target_property(rf_sources morse_rf_boundary SOURCES)
 set(rf_wrappers)
 set(index 0)
 foreach(source IN LISTS rf_sources)
  if(source MATCHES "\\.c$")
   math(EXPR index "${index}+1")
   get_filename_component(basename "${source}" NAME)
   set(wrapper "${CMAKE_CURRENT_BINARY_DIR}/companion_rf/${index}_${basename}")
   file(WRITE "${wrapper}" "/* Generated RF-only translation unit. */\n#include \"${source}\"\n")
   list(APPEND rf_wrappers "${wrapper}")
  endif()
 endforeach()
 set_property(TARGET morse_rf_boundary PROPERTY SOURCES "${rf_wrappers}")
 # Each driver's HAL selection must return its own operation table.
 file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/companion_chip.c"
   "#include \"mmhal_wlan.h\"\nconst struct mmhal_chip *mmhal_get_chip(void) { return &mmhal_mm6108; }\n")
 target_sources(morse_rf_boundary PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/companion_chip.c")
 target_compile_definitions(morse_rf_boundary PRIVATE
  mmhal_get_chip=ez_companion_get_chip
    mmhal_mm6108=ez_companion_mmhal_mm6108
    mmhal_mm8108=ez_companion_mmhal_mm8108
    morse_xtal_init_delay=ez_companion_xtal_init_delay
  mmhal_spi_irq_poll_interval=ez_companion_spi_irq_poll_interval)
 target_link_libraries(edgez_umac_runtime PRIVATE edgez_halow_mesh_core)
 # The station archive is already namespaced by its supported mangler.
 get_target_property(morse_binary libmorse BINARY_DIR)
 set_property(TARGET morselib PROPERTY INTERFACE_LINK_LIBRARIES
  "-Wl,--start-group;${morse_binary}/libmorse.a;shims;mmpktmem;mmutils;-Wl,--end-group")
 target_sources(${COMPONENT_LIB} PRIVATE "${common}/companion_mesh_protocol.c"
  "${common}/companion_mesh_radio.c" "${CMAKE_CURRENT_LIST_DIR}/dev01_mesh.c")
 target_include_directories(${COMPONENT_LIB} PRIVATE ${MM_IOT_EDGEZ_INCLUDE_DIRS} ${EDGEZ_RF_API_INCLUDE_DIRS})
 # Keep the mesh HostAP hooks separate from the station archive's mmint ABI.
 get_target_property(platform_archive edgez_halow_mesh_platform IMPORTED_LOCATION)
 get_filename_component(platform_name "${platform_archive}" NAME)
 set(crypto_output "${CMAKE_CURRENT_BINARY_DIR}/companion_crypto")
 set(namespace_script "${CMAKE_CURRENT_LIST_DIR}/../../scripts/namespace_companion_crypto.py")
 add_custom_command(OUTPUT "${crypto_output}/${platform_name}"
  COMMAND "${Python3_EXECUTABLE}" "${namespace_script}" --nm "${CMAKE_NM}"
   --objcopy "${CMAKE_OBJCOPY}" --output "${crypto_output}" "${platform_archive}"
  DEPENDS "${namespace_script}" "${platform_archive}")
 add_custom_target(companion_crypto_archives DEPENDS "${crypto_output}/${platform_name}")
 set_target_properties(edgez_halow_mesh_platform PROPERTIES IMPORTED_LOCATION "${crypto_output}/${platform_name}")
 add_dependencies(edgez_halow_mesh_platform companion_crypto_archives)
 target_link_libraries(${COMPONENT_LIB} PUBLIC edgez_umac_runtime morse_rf_boundary edgez_halow_mesh_platform)
endfunction()
add_companion_radio()

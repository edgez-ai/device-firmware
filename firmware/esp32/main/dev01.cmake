# Dependency discovery precedes Kconfig and runs in CMake script mode.
if(CMAKE_SCRIPT_MODE_FILE)
    idf_component_register(PRIV_REQUIRES halow bt esp_wifi esp_event esp_netif
        esp_timer esp_pm esp_app_format esp_driver_gpio esp_driver_rmt esp_driver_uart esp_hw_support vfs nvs_flash spi_flash esp_partition mbedtls json)
    return()
endif()
set(common "${CMAKE_CURRENT_LIST_DIR}/../../src")
set(wakaama "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/wakaama")
file(GLOB wakaama_sources CONFIGURE_DEPENDS "${wakaama}/core/*.c" "${wakaama}/data/*.c" "${wakaama}/coap/*.c" "${wakaama}/coap/er-coap-13/*.c")
idf_component_register(SRCS "dev01_main.c" "dev01_ble.c" "dev01_lwm2m.c" "dev01_scan.c" "dev01_diag.c" "dev01_diag_format.c" "dev01_power.c" "dev01_sleep.c" "dev01_led.c" "dev01_observe.c" "edgez_esp_factory_data.c"
 "${common}/factory_record.c" "${common}/prov_protocol.c" "${common}/companion_protocol.c" ${wakaama_sources}
 "${wakaama}/examples/client/common/object_security.c" "${wakaama}/examples/client/common/object_server.c"
 INCLUDE_DIRS "include" "${common}" "${wakaama}/include" "${wakaama}/core" "${wakaama}/coap" "${wakaama}/examples/client/common"
 PRIV_REQUIRES halow bt esp_wifi esp_event esp_netif esp_timer esp_pm esp_app_format esp_driver_gpio esp_driver_rmt esp_driver_uart esp_hw_support vfs nvs_flash spi_flash esp_partition mbedtls json)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LWM2M_CLIENT_MODE LWM2M_LITTLE_ENDIAN LWM2M_SUPPORT_TLV LWM2M_SUPPORT_JSON LWM2M_SUPPORT_SENML_JSON LWM2M_COAP_DEFAULT_BLOCK_SIZE=256 LWM2M_COAP_MAX_MESSAGE_SIZE=1024 LWM2M_COAP_DEFAULT_MAX_RETRANSMIT=4 LWM2M_COAP_SEPARATE_TIMEOUT=15)
# Keep the upstream netif/driver implementation; select the factory region
# before boot instead of briefly starting RF in a compiled default domain.
idf_component_get_property(halow_component halow COMPONENT_LIB)
get_target_property(halow_sources ${halow_component} SOURCES)
list(FILTER halow_sources EXCLUDE REGEX "[/\\\\]mmhalow\\.c$")
set_property(TARGET ${halow_component} PROPERTY SOURCES "${halow_sources}")
file(READ "${CMAKE_CURRENT_LIST_DIR}/../platforms/esp-halow/halow/mmhalow.c" netif_source)
string(REPLACE "CONFIG_HALOW_COUNTRY_CODE" "dev01_factory_country()" netif_source "${netif_source}")
string(REPLACE "    mmwlan_init();" "    mmwlan_init();\n#if !CONFIG_EZ_DEV01_AUTO_SLEEP\n    mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED);\n#endif" netif_source "${netif_source}")
set(netif_source "extern const char *dev01_factory_country(void);\n${netif_source}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dev01_mmhalow.c" "${netif_source}")
target_sources(${halow_component} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/dev01_mmhalow.c")
# Retain the HC33 reboot-safe GPIO/IRQ initialization already used by the template.
get_target_property(shims_sources shims SOURCES)
list(FILTER shims_sources EXCLUDE REGEX "mmhal_os\\.c$")
set_property(TARGET shims PROPERTY SOURCES "${shims_sources}")
target_sources(shims PRIVATE "${CMAKE_CURRENT_LIST_DIR}/mmhal_os_ps_override.c")

# ESP-IDF 5.5.4's no-local-Wi-Fi component still includes an AP-only helper
# whose native Wi-Fi types are unavailable. This station never uses that API.
idf_component_get_property(wifi_component esp_wifi COMPONENT_LIB)
get_target_property(wifi_sources ${wifi_component} SOURCES)
list(FILTER wifi_sources EXCLUDE REGEX "wifi_default_ap\\.c$")
set_property(TARGET ${wifi_component} PROPERTY SOURCES "${wifi_sources}")

# Coordinate the external radio HAL with automatic MCU light sleep, without
# modifying the pinned esp-halow checkout.
get_target_property(power_shims_sources shims SOURCES)
list(FILTER power_shims_sources EXCLUDE REGEX "mmhal_(core|wlan)\\.c$")
set_property(TARGET shims PROPERTY SOURCES "${power_shims_sources}")
set(shim_dir "${CMAKE_CURRENT_LIST_DIR}/../platforms/esp-halow/halow/components/shims")
file(READ "${shim_dir}/mmhal_core.c" power_core)
string(REPLACE "MM_UNUSED(veto_id);" "dev01_power_veto(veto_id, true);" power_core "${power_core}")
string(REPLACE "void mmhal_clear_deep_sleep_veto(uint8_t veto_id)\n{\n    dev01_power_veto(veto_id, true);" "void mmhal_clear_deep_sleep_veto(uint8_t veto_id)\n{\n    dev01_power_veto(veto_id, false);" power_core "${power_core}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dev01_mmhal_core.c" "#include <stdint.h>\n#include <stdbool.h>\nextern void dev01_power_veto(uint8_t, bool);\n${power_core}")
file(READ "${shim_dir}/mmhal_wlan.c" power_wlan)
string(REPLACE "    gpio_set_level(CONFIG_MM_WAKE, 1);" "    dev01_power_radio_awake(true);\n    gpio_set_level(CONFIG_MM_WAKE, 1);" power_wlan "${power_wlan}")
string(REPLACE "    gpio_set_level(CONFIG_MM_WAKE, 0);" "    gpio_set_level(CONFIG_MM_WAKE, 0);\n    dev01_power_radio_awake(false);" power_wlan "${power_wlan}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dev01_mmhal_wlan.c" "#include <stdbool.h>\nextern void dev01_power_radio_awake(bool);\n${power_wlan}")
target_sources(shims PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/dev01_mmhal_core.c" "${CMAKE_CURRENT_BINARY_DIR}/dev01_mmhal_wlan.c")

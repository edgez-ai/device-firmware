/* EZ-DEV54: common services with factory/persisted external hardware selection. */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "halow_sta.h"
#include "hardware_config.h"
#include "hardware_uart.h"
#include "hardware_memory.h"
#include "ble_prov.h"
#include "radar_app.h"
#include "radar_mqtt.h"
#if defined(CONFIG_EZ_DEV54_SENSORS)
#include "sensors_hw.h"
#endif

LOG_MODULE_REGISTER(main, CONFIG_RADAR_LOG_LEVEL);

int main(void)
{
	(void)hardware_config_init();
	if (hardware_uart_prepare()) return -1;
	int memory_rc = hardware_memory_init();
	if (memory_rc) LOG_ERR("Peripheral memory allocation failed: %d", memory_rc);
#if defined(CONFIG_EZ_DEV54_BLE_PROV)
	int prov_rc = ez_prov_init();
	if (prov_rc) LOG_ERR("Provisioning init failed: %d", prov_rc);
#endif
	int rc = radar_app_start();

	if (rc != 0) {
		LOG_ERR("radar start failed: %d", rc);
	}
#if defined(CONFIG_EZ_DEV54_SENSORS)
	int sensors_rc = board_sensors_init();
	if (sensors_rc) LOG_WRN("Sensor standby initialization: %d", sensors_rc);
#endif
	(void)halow_sta_start();
	(void)radar_mqtt_start();
	if (CONFIG_RADAR_STATUS_LOG_SECONDS == 0) {
		return 0;
	}

	for (;;) {
		struct radar_status status;

		k_sleep(K_SECONDS(CONFIG_RADAR_STATUS_LOG_SECONDS));
		radar_app_get_status(&status);
		if (status.valid) {
			LOG_INF("radar %s: present %u, range %u cm, points %u; restarts %u; "
				"HaLow %s",
				status.running ? "running" : "stopped", status.result.present,
				(unsigned int)(status.result.range_m * 100.0f),
				status.result.num_points, status.restarts,
				halow_sta_is_online() ? "online" : "offline");
		} else {
			LOG_INF("radar: no data yet (restarts %u); HaLow %s",
				status.restarts,
				halow_sta_is_online() ? "online" : "offline");
		}
	}
	return 0;
}

/*
 * nRF54L15 + FGH200M host for the IWRL6432 radar.
 *
 *  - uses radar firmware flashed separately over USB;
 *  - receives the autostart radar's output without sending UART commands;
 *  - decodes the radar's result (0/1, range, cluster, points) from the UART;
 *  - joins a Wi-Fi HaLow access point as a station with DTIM power save.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "halow_sta.h"
#include "ble_prov.h"
#include "radar_app.h"
#include "radar_mqtt.h"

LOG_MODULE_REGISTER(main, CONFIG_RADAR_LOG_LEVEL);

int main(void)
{
#if defined(CONFIG_EZ_CAM_BLE_PROV)
	int prov_rc = ez_prov_init();
	if (prov_rc) LOG_ERR("Provisioning init failed: %d", prov_rc);
#endif
	int rc = radar_app_start();

	if (rc != 0) {
		LOG_ERR("radar start failed: %d", rc);
	}
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

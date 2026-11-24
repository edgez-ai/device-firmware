/* Radar supervisor. See radar_app.h. */
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_RADAR_SIMULATE_PRESENCE) || defined(RADAR_HALOW_LIGHT)
#include <zephyr/drivers/gpio.h>
#endif
#if defined(CONFIG_RADAR_SIMULATE_PRESENCE)
#include <hal/nrf_uarte.h>
#endif

#include "radar_app.h"
#include "ble_prov.h"
#include "halow_light.h"
#ifdef RADAR_HALOW_CAMERA
#include "halow_camera.h"
#endif
#if defined(CONFIG_EZ_CAM_UART_GPS)
#include "halow_gps.h"
#endif
#include "radar_link.h"
#include "radar_mqtt.h"
#ifdef CONFIG_RADAR_CONFIGURE_SENSOR
#include "iwrl6432_cli.h"
#include "radar_config.inc"
#endif

LOG_MODULE_REGISTER(radar, CONFIG_RADAR_LOG_LEVEL);


static K_THREAD_STACK_DEFINE(radar_stack, CONFIG_RADAR_THREAD_STACK_SIZE);
static struct k_thread radar_thread;
static K_MUTEX_DEFINE(status_lock);
static struct radar_status status;
#if !defined(CONFIG_RADAR_SIMULATE_PRESENCE)
static struct iwrl_stream stream;
#endif



__weak void radar_app_on_result(const struct iwrl_human_result *result, bool changed)
{
	if (changed) {
		/* Range in centimetres: the log needs no floating-point support. */
		LOG_INF("frame %u: %u (%s) range %u cm, cluster_now %u, points %u",
			result->frame_number, result->present,
			result->present ? "HUMAN" : "nobody",
			(unsigned int)(result->range_m * 100.0f), result->cluster_now,
			result->num_points);
	}
}

static void on_result(const struct iwrl_human_result *result, void *user)
{
	bool changed;

	ARG_UNUSED(user);
	k_mutex_lock(&status_lock, K_FOREVER);
	changed = !status.valid || (status.result.present != result->present);
	status.result = *result;
	status.valid = true;
	status.running = true;
	status.result_uptime_ms = k_uptime_get();
	k_mutex_unlock(&status_lock);

	if (changed) {
		radar_mqtt_update(result);
	}
#if defined(RADAR_HALOW_LIGHT) && !defined(CONFIG_RADAR_SIMULATE_PRESENCE)
	halow_light_radar_sample(result->present, result->range_m);
#endif
	radar_app_on_result(result, changed);
}

#if defined(CONFIG_RADAR_SIMULATE_PRESENCE) || defined(RADAR_HALOW_LIGHT)
static const struct gpio_dt_spec sim_button =
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec sim_led =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), presence_test_led_gpios);
static int sim_initial_button;
static int sim_stable_button;
static struct gpio_callback sim_button_cb;
static struct k_work_delayable sim_debounce;
static K_SEM_DEFINE(sim_presses, 0, 16);

static void simulation_debounced(struct k_work *work)
{
	int sample = gpio_pin_get_dt(&sim_button);

	ARG_UNUSED(work);
	if (sample >= 0 && sample != sim_stable_button) {
		sim_stable_button = sample;
#if defined(CONFIG_EZ_CAM_BLE_PROV)
		ez_prov_button_changed(sample != 0);
#endif
#ifdef RADAR_HALOW_LIGHT
		halow_light_button_changed(sample != 0);
#else
		if (sample != 0) {
			k_sem_give(&sim_presses);
		}
#endif
	}
}

static void simulation_button_edge(const struct device *dev,
				   struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	/* Each bounce restarts the quiet interval; no periodic button poll. */
	k_work_reschedule(&sim_debounce, K_MSEC(30));
}

static int button_init(void)
{
	int rc;

	if (!gpio_is_ready_dt(&sim_button) || !gpio_is_ready_dt(&sim_led)) {
		return -ENODEV;
	}
#if defined(CONFIG_EZ_CAM_UART_BASE)
	/* Explicitly release old UART state too, including after debug resets.
	 * The simulator has no serial driver/device and does not drive TX/RX. */
	nrf_uarte_disable((NRF_UARTE_Type *)DT_REG_ADDR(DT_NODELABEL(uart22)));
	rc = gpio_pin_configure(sim_button.port, 11, GPIO_DISCONNECTED);
	if (rc == 0) {
		rc = gpio_pin_configure(sim_button.port, 12, GPIO_DISCONNECTED);
	}
	if (rc != 0) {
		return rc;
	}
	LOG_INF("SIMULATION: UART22 disabled; P1.11/P1.12 disconnected");
	LOG_INF("SIMULATION: radar power enable P1.09 %s via overlay GPIO hog",
		DT_NODE_HAS_PROP(DT_NODELABEL(radar_power_off), output_high) ? "HIGH" : "LOW");
#endif
	rc = gpio_pin_configure_dt(&sim_button, GPIO_INPUT);
	if (rc != 0) {
		return rc;
	}
	rc = gpio_pin_configure_dt(&sim_led, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		return rc;
	}
	sim_initial_button = gpio_pin_get_dt(&sim_button);
	if (sim_initial_button < 0) {
		return sim_initial_button;
	}
	sim_stable_button = sim_initial_button;
#if defined(CONFIG_EZ_CAM_BLE_PROV)
	ez_prov_button_changed(sim_stable_button != 0);
#endif
	k_work_init_delayable(&sim_debounce, simulation_debounced);
	gpio_init_callback(&sim_button_cb, simulation_button_edge, BIT(sim_button.pin));
	rc = gpio_add_callback(sim_button.port, &sim_button_cb);
	if (rc != 0) {
		return rc;
	}
	rc = gpio_pin_interrupt_configure_dt(&sim_button, GPIO_INT_EDGE_BOTH);
	if (rc == 0) {
		/* Recheck after enabling IRQ to cover an edge during setup. */
		k_work_reschedule(&sim_debounce, K_MSEC(30));
	}
	return rc;
}

#endif

#if defined(CONFIG_RADAR_SIMULATE_PRESENCE)
static void simulate_results(void)
{
	struct iwrl_human_result result = {0};
	bool present = false;

	LOG_INF("SIMULATION: interrupt-driven KEY P1.08; LED indication %s",
		IS_ENABLED(CONFIG_RADAR_SIMULATION_LED) ? "enabled" : "off");
	LOG_INF("SIMULATION: initial nobody; results only on changes; radar UART unused");
	for (;;) {
		result.frame_number++;
		result.present = present;
		result.cluster_now = present;
		result.num_points = present ? 3U : 0U;
		result.range_m = present ? 1.0f : 0.0f;
		on_result(&result, NULL);
		k_sem_take(&sim_presses, K_FOREVER);
		present = !present;
		if (IS_ENABLED(CONFIG_RADAR_SIMULATION_LED)) {
			int rc = gpio_pin_set_dt(&sim_led, present);
			if (rc != 0) {
				LOG_WRN("simulation LED update failed: %d", rc);
			}
		}
	}
}
#elif defined(CONFIG_RADAR_CONFIGURE_SENSOR)
/* Decode output of the configured radar; the helpers below start it first. */
static void trace_cli(const char *command, const char *reply, void *user)
{
	ARG_UNUSED(user);
	LOG_DBG("> %s", command);
	if (strstr(reply, "Error") != NULL || strstr(reply, "not recognized") != NULL) {
		LOG_ERR("radar rejected \"%s\": %s", command, reply);
	}
}

/* Power-cycle the radar into its application and send the configuration.
 * The radar firmware does not start by itself: it waits for these commands. */
static int start_radar(void)
{
	const struct iwrl_port *port = radar_link_port();
	int rc;

	rc = radar_link_power_cycle();
	if (rc != 0) {
		return rc;
	}
	/* The banner and first prompt were printed while the radar booted; an
	 * empty line makes the CLI print its prompt again. */
	rc = iwrl_cli_wait_ready(port, 1, CONFIG_RADAR_BOOT_TIMEOUT_MS);
	if (rc != 0) {
		rc = iwrl_cli_wait_ready(port, 1, CONFIG_RADAR_BOOT_TIMEOUT_MS);
	}
	if (rc != 0) {
		LOG_ERR("radar CLI does not answer (%d)", rc);
		return rc;
	}
	rc = iwrl_cli_send_config(port, (const char *)radar_config, radar_config_size,
				  CONFIG_RADAR_CLI_TIMEOUT_MS, trace_cli, NULL);
	if (rc != 0) {
		LOG_ERR("radar configuration failed (%d)", rc);
		return rc;
	}
	iwrl_stream_init(&stream);
	LOG_INF("radar configured and started");
	return 0;
}

/* Decode reports until the radar has been silent for too long: the first
 * report must arrive within CONFIG_RADAR_WATCHDOG_MS, every later one (a
 * change or the heartbeat) within CONFIG_RADAR_HEARTBEAT_TIMEOUT_S.
 *
 * With wake-up on the RX line the UART is switched off once the first report
 * has shown that the radar runs. The radar's wake byte (a low pulse on RX)
 * then switches it on for one report at a time. */
static void read_reports(void)
{
	const struct iwrl_port *port = radar_link_port();
	int64_t deadline = k_uptime_get() + CONFIG_RADAR_WATCHDOG_MS;
	uint32_t seen = stream.results;
	bool gated = false;
	uint32_t empty_wakes = 0;
	uint8_t chunk[64];
	int got;

	for (;;) {
		int64_t now = k_uptime_get();

		if (now >= deadline) {
			return;
		}
		if (gated) {
			int64_t window_end;
			int woke = radar_link_wait_wake((uint32_t)MIN(deadline - now, 60000));

			if (woke < 0) {
				LOG_ERR("radar RX wake failed: %d", woke);
				k_msleep(1000);
				continue;
			}
			if (woke == 0) {
				continue;
			}
			if (radar_link_rx_enable(true) != 0) {
				return;
			}
			/* The report follows the wake byte after the radar's lead
			 * time. Read until it has been decoded and the line is
			 * quiet, or the window ends without one. */
			window_end = k_uptime_get() + CONFIG_RADAR_WAKE_WINDOW_MS;
			do {
				got = port->read(port->ctx, chunk, sizeof(chunk),
						 CONFIG_RADAR_WAKE_LINGER_MS);
				if (got > 0) {
					iwrl_stream_feed(&stream, chunk, (size_t)got, on_result,
							 NULL);
				}
			} while (k_uptime_get() < window_end &&
				 (got > 0 || stream.results == seen));
			if (radar_link_rx_enable(false) != 0) {
				return;
			}
			if (stream.results == seen) {
				/* Noise, a line held low, or a report that was lost. */
				empty_wakes++;
				if ((empty_wakes == 1U) || ((empty_wakes % 100U) == 0U)) {
					LOG_WRN("RX wake without a report (%u so far)",
						empty_wakes);
				}
			}
		} else {
			got = port->read(port->ctx, chunk, sizeof(chunk), 200U);
			if (got > 0) {
				iwrl_stream_feed(&stream, chunk, (size_t)got, on_result, NULL);
			}
		}
		if (stream.results != seen) {
			seen = stream.results;
			deadline = k_uptime_get() +
				   (int64_t)CONFIG_RADAR_HEARTBEAT_TIMEOUT_S * 1000;
			if (!gated && radar_link_has_wake()) {
				if (radar_link_rx_enable(false) != 0) {
					return;
				}
				gated = true;
				LOG_INF("first report received; UART off until the RX line wakes it");
			}
		}
	}
}
#else
/* Decode output until the radar has been silent for the watchdog time. */
static void read_results(void)
{
	const struct iwrl_port *port = radar_link_port();
	int64_t last_result = k_uptime_get();
	uint32_t seen = stream.results;
	uint8_t chunk[64];

	for (;;) {
		int got = port->read(port->ctx, chunk, sizeof(chunk), 200U);

		if (got > 0) {
			iwrl_stream_feed(&stream, chunk, (size_t)got, on_result, NULL);
		}
		if (stream.results != seen) {
			seen = stream.results;
			last_result = k_uptime_get();
		} else if ((k_uptime_get() - last_result) > CONFIG_RADAR_WATCHDOG_MS) {
			return;
		}
	}
}
#endif

static void radar_thread_fn(void *a, void *b, void *c)
{

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

#if defined(CONFIG_RADAR_SIMULATE_PRESENCE)
	simulate_results();
#elif defined(CONFIG_RADAR_CONFIGURE_SENSOR)
	for (;;) {
		if (start_radar() == 0) {
			read_reports();
			LOG_WRN("no radar report in time; restarting the radar");
		}
		radar_link_log_diagnostics();

		k_mutex_lock(&status_lock, K_FOREVER);
		status.running = false;
		k_mutex_unlock(&status_lock);
		k_msleep(CONFIG_RADAR_RETRY_DELAY_MS);
	}
#else
	iwrl_stream_init(&stream);
	LOG_INF("receive-only: waiting for radar autostart TLV 1101");
	for (;;) {
		read_results();
		LOG_WRN("no valid radar frame for %d ms; continuing receive-only",
			CONFIG_RADAR_WATCHDOG_MS);
		radar_link_log_diagnostics();

		k_mutex_lock(&status_lock, K_FOREVER);
		status.running = false;
		k_mutex_unlock(&status_lock);
	}
#endif
}

void radar_app_get_status(struct radar_status *out)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	*out = status;
	k_mutex_unlock(&status_lock);
}

int radar_app_start(void)
{
	int rc;

#if defined(CONFIG_RADAR_SIMULATE_PRESENCE) || defined(RADAR_HALOW_LIGHT)
	rc = button_init();
	if (rc != 0) {
		return rc;
	}
#endif
#ifdef RADAR_HALOW_CAMERA
	rc = halow_camera_start();
	if (rc != 0) {
		LOG_ERR("Camera initialization failed: %d", rc);
	}
#endif
#ifdef RADAR_HALOW_LIGHT

#if defined(CONFIG_EZ_CAM_UART_GPS)
	rc = halow_gps_init();
	if (rc != 0) LOG_ERR("GPS initialization failed: %d", rc);
#endif
	rc = halow_light_start(sim_stable_button != 0);
	if (rc != 0 || IS_ENABLED(CONFIG_RADAR_SIMULATE_PRESENCE)) {
		return rc;
	}
#endif
#if !defined(CONFIG_RADAR_SIMULATE_PRESENCE)
	rc = radar_link_init();
	if (rc != 0) {
		return rc;
	}
#endif
	k_thread_create(&radar_thread, radar_stack, K_THREAD_STACK_SIZEOF(radar_stack),
			radar_thread_fn, NULL, NULL, NULL, CONFIG_RADAR_THREAD_PRIORITY, 0,
			K_NO_WAIT);
	k_thread_name_set(&radar_thread, "radar");
	return 0;
}

/* Wi-Fi HaLow station with DTIM power save. See halow_sta.h. */
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#ifdef RADAR_HALOW_COAP_UDP
#include <zephyr/net/dhcpv4.h>
#endif
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_ip.h>

#include <mmwlan.h>

#include "halow_sta.h"
#include "ble_prov.h"
#include "halow_light.h"

LOG_MODULE_REGISTER(halow_sta, CONFIG_RADAR_LOG_LEVEL);

static struct net_mgmt_event_callback wifi_cb;
#if defined(CONFIG_NET_IPV4)
static struct net_mgmt_event_callback ipv4_cb;
#endif
static struct k_work_delayable connect_work;
static bool associated;
static bool have_address;
static int64_t connect_started_ms;

/* Applied before each connect request. The listen interval can only be set
 * while the station is disabled, so a refusal on a retry keeps the value
 * accepted earlier. */
static void apply_idle_tuning(void)
{
	static bool listen_interval_set;
	static bool health_check_set;
	enum mmwlan_status status;

	if ((CONFIG_RADAR_HALOW_LISTEN_INTERVAL > 0) && !listen_interval_set) {
		status = mmwlan_set_listen_interval(CONFIG_RADAR_HALOW_LISTEN_INTERVAL);
		if (status == MMWLAN_SUCCESS) {
			listen_interval_set = true;
			LOG_INF("HaLow listen interval %d beacons",
				CONFIG_RADAR_HALOW_LISTEN_INTERVAL);
		} else {
			LOG_WRN("could not set HaLow listen interval (%d)", (int)status);
		}
	}
	if ((CONFIG_RADAR_HALOW_HEALTH_CHECK_SECONDS >= 0) && !health_check_set) {
		uint32_t period_ms = (uint32_t)CONFIG_RADAR_HALOW_HEALTH_CHECK_SECONDS * 1000U;

		status = mmwlan_set_health_check_interval(period_ms, period_ms);
		if (status == MMWLAN_SUCCESS) {
			health_check_set = true;
			LOG_INF("HaLow health check every %d s",
				CONFIG_RADAR_HALOW_HEALTH_CHECK_SECONDS);
		} else {
			LOG_WRN("could not set HaLow health check period (%d)", (int)status);
		}
	}
}

static void request_connect(struct k_work *work)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_connect_req_params params = { 0 };
	int rc;

	ARG_UNUSED(work);
	if (iface == NULL) {
		LOG_ERR("no Wi-Fi HaLow interface");
		return;
	}

#if defined(CONFIG_EZ_CAM_BLE_PROV)
	char ssid[33], password[64];
	if (!ez_prov_get_credentials(ssid, password)) return;
#else
	const char *ssid = CONFIG_RADAR_HALOW_SSID;
	const char *password = CONFIG_RADAR_HALOW_PASSWORD;
#endif
	params.ssid = (const uint8_t *)ssid;
	params.ssid_length = (uint8_t)strlen(ssid);
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_UNKNOWN;
	/* This driver ignores params.channel for station connections. Configure
	 * the Morse scan engine directly, using S1G operating channel numbers. */
	if (CONFIG_RADAR_HALOW_CHANNEL != 0) {
		uint8_t channel = CONFIG_RADAR_HALOW_CHANNEL;
		struct mmwlan_scan_config scan = MMWLAN_SCAN_CONFIG_INIT;

		scan.selected_channels = &channel;
		scan.selected_channels_len = 1;
		scan.selective_scan_attempts = UINT8_MAX;
		if (mmwlan_set_scan_config(&scan) != MMWLAN_SUCCESS) {
			LOG_ERR("could not select HaLow channel %u", channel);
			k_work_reschedule(&connect_work, K_SECONDS(CONFIG_RADAR_HALOW_RETRY_SECONDS));
			return;
		}
		LOG_INF("HaLow fixed scan: region %s, S1G channel %u",
			CONFIG_WIFI_MORSEMICRO_REGION, channel);
	}
	if (strlen(password) > 0U) {
		params.security = WIFI_SECURITY_TYPE_SAE;
		params.sae_password = (const uint8_t *)password;
		params.sae_password_length = (uint8_t)strlen(password);
		params.mfp = WIFI_MFP_REQUIRED;
	} else {
		params.security = WIFI_SECURITY_TYPE_NONE;
		params.mfp = WIFI_MFP_DISABLE;
	}

	if (IS_ENABLED(CONFIG_RADAR_HALOW_DUTY_CYCLE_BURST)) {
		enum mmwlan_status status = mmwlan_set_duty_cycle_mode(MMWLAN_DUTY_CYCLE_MODE_BURST);

		if (status != MMWLAN_SUCCESS) {
			LOG_WRN("could not enable HaLow duty-cycle burst mode (%d)", (int)status);
		}
	}
	apply_idle_tuning();
	connect_started_ms = k_uptime_get();
	rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
	if (rc != 0) {
		LOG_WRN("HaLow connect request failed (%d), retrying", rc);
		k_work_reschedule(&connect_work, K_SECONDS(CONFIG_RADAR_HALOW_RETRY_SECONDS));
	} else {
		LOG_INF("connecting to HaLow AP \"%s\"", CONFIG_RADAR_HALOW_SSID);
	}
}

/* The station sleeps between DTIM beacons and wakes for buffered traffic. The
 * DTIM period itself is set by the access point. */
static void enable_dtim_power_save(void)
{
#if defined(CONFIG_WIFI_MORSEMICRO_POWERSAVE)
	enum mmwlan_status status = mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);

	if (status == MMWLAN_SUCCESS) {
		status = mmwlan_set_dynamic_ps_timeout(CONFIG_RADAR_HALOW_DYNAMIC_PS_TIMEOUT_MS);
	}
	if (status != MMWLAN_SUCCESS) {
		LOG_WRN("could not enable HaLow power save (%d)", (int)status);
	} else {
		LOG_INF("HaLow DTIM power save enabled; dynamic idle %u ms",
			CONFIG_RADAR_HALOW_DYNAMIC_PS_TIMEOUT_MS);
	}
#else
	LOG_WRN("HaLow power save needs the busy and wake-up lines in the devicetree");
#endif
}

static void on_wifi_event(struct net_mgmt_event_callback *cb, uint64_t event,
			  struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *result = cb->info;

		if ((result != NULL) && (result->status == 0)) {
			associated = true;
			LOG_INF("HaLow associated after %lld ms",
				(long long)(k_uptime_get() - connect_started_ms));
			enable_dtim_power_save();
#ifdef RADAR_HALOW_COAP_UDP
			net_dhcpv4_start(iface);
#else
			halow_light_link_changed(true);
#endif
		} else {
			associated = false;
#if defined(CONFIG_EZ_CAM_BLE_PROV)
			ez_prov_wifi_failed(result ? result->status : -EIO);
			if (!ez_prov_retry_allowed()) return;
#endif
			LOG_WRN("HaLow association failed, retrying");
			k_work_reschedule(&connect_work,
					  K_SECONDS(CONFIG_RADAR_HALOW_RETRY_SECONDS));
		}
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		associated = false;
		have_address = false;
		halow_light_link_changed(false);
#ifdef RADAR_HALOW_COAP_UDP
		net_dhcpv4_stop(iface);
#endif
		LOG_WRN("HaLow disconnected, reconnecting");
		k_work_reschedule(&connect_work, K_SECONDS(CONFIG_RADAR_HALOW_RETRY_SECONDS));
	}
}

#if defined(CONFIG_NET_IPV4)
static void on_ipv4_event(struct net_mgmt_event_callback *cb, uint64_t event,
			  struct net_if *iface)
{
	ARG_UNUSED(cb);
	if (iface != net_if_get_first_wifi()) {
		return;
	}

	if (event == NET_EVENT_IPV4_ADDR_ADD) {
		have_address = true;
		LOG_INF("HaLow IPv4 address assigned");
	} else if (event == NET_EVENT_IPV4_ADDR_DEL) {
		have_address = false;
#ifdef RADAR_HALOW_COAP_UDP
		halow_light_link_changed(false);
#endif
	} else if (event == NET_EVENT_IPV4_DHCP_BOUND) {
#if defined(CONFIG_EZ_CAM_BLE_PROV)
		ez_prov_wifi_bound(iface);
#endif
		struct net_in_addr current = net_if_ipv4_get_gw(iface);
		struct net_in_addr fallback;

		if (current.s_addr == 0 && CONFIG_RADAR_HALOW_GATEWAY[0] != '\0' &&
		    net_addr_pton(AF_INET, CONFIG_RADAR_HALOW_GATEWAY, &fallback) == 0) {
			net_if_ipv4_set_gw(iface, &fallback);
			LOG_INF("HaLow using fallback gateway %s", CONFIG_RADAR_HALOW_GATEWAY);
		}
#ifdef RADAR_HALOW_COAP_UDP
		if (associated) {
			halow_light_link_changed(true);
		}
#endif
	}
}

#endif

bool halow_sta_is_online(void)
{
	return associated && have_address;
}

int halow_sta_start(void)
{
#if defined(RADAR_HALOW_LIGHT) && !defined(CONFIG_EZ_CAM_BLE_PROV)
	if (strlen(CONFIG_RADAR_HALOW_PASSWORD) < 8U) {
		LOG_ERR("HaLow Light Control requires a configured WPA3 password");
		return -EINVAL;
	}
#endif
	if (!IS_ENABLED(CONFIG_EZ_CAM_BLE_PROV) && strlen(CONFIG_RADAR_HALOW_SSID) == 0U) {
		LOG_WRN("CONFIG_RADAR_HALOW_SSID is empty: HaLow stays idle");
		return -ENOTSUP;
	}

	net_mgmt_init_event_callback(&wifi_cb, on_wifi_event,
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
#if defined(CONFIG_NET_IPV4)
	net_mgmt_init_event_callback(&ipv4_cb, on_ipv4_event,
				     NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL |
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&ipv4_cb);
#endif

	k_work_init_delayable(&connect_work, request_connect);
	k_work_reschedule(&connect_work, K_SECONDS(1));
	return 0;
}

#if defined(CONFIG_EZ_CAM_BLE_PROV)
static void reconnect_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	struct net_if *iface = net_if_get_first_wifi();
	if (!iface) return;
	(void)k_work_cancel_delayable(&connect_work);
	associated = false; have_address = false;
	halow_light_link_changed(false);
	net_dhcpv4_stop(iface);
	(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	(void)k_work_reschedule(&connect_work, K_MSEC(500));
}
K_WORK_DEFINE(reconnect_work, reconnect_handler);
int halow_sta_reconnect(void)
{
	return k_work_submit(&reconnect_work) < 0 ? -EIO : 0;
}
#endif

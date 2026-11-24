/* MQTT 3.1.1 presence publisher; all network I/O lives in its own thread. */
#include "radar_mqtt.h"

#if defined(CONFIG_RADAR_MQTT_ENABLED)
#include <errno.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/sys/eventfd.h>
#include <zephyr/sys/printk.h>

#include "halow_sta.h"
#include "radar_mqtt.h"

LOG_MODULE_REGISTER(radar_mqtt, CONFIG_RADAR_LOG_LEVEL);

struct presence_message {
	uint32_t sequence;
	uint32_t frame;
	uint32_t uptime_ms;
	uint8_t present;
};

K_MSGQ_DEFINE(changes, sizeof(struct presence_message), 16, 4);
static K_MUTEX_DEFINE(state_lock);
static struct presence_message latest;
static bool have_latest;
static K_THREAD_STACK_DEFINE(mqtt_stack, 3072);
static struct k_thread mqtt_thread;
static struct mqtt_client client;
static struct sockaddr_in broker;
static uint8_t rx_buf[256], tx_buf[256];
static char client_id[32], topic[128], payload[192];
static struct mqtt_utf8 username = {
	.utf8 = (uint8_t *)CONFIG_RADAR_MQTT_USERNAME,
	.size = sizeof(CONFIG_RADAR_MQTT_USERNAME) - 1,
};
static struct mqtt_utf8 password = {
	.utf8 = (uint8_t *)CONFIG_RADAR_MQTT_PASSWORD,
	.size = sizeof(CONFIG_RADAR_MQTT_PASSWORD) - 1,
};
/* The following state is owned exclusively by the MQTT thread/callback. */
static bool connected, failed, resync, pending;
static uint16_t next_id, pending_id;
static int64_t ack_deadline;
static int change_fd = -1;
static struct presence_message in_flight;

void radar_mqtt_update(const struct iwrl_human_result *result)
{
	struct presence_message discarded;

	k_mutex_lock(&state_lock, K_FOREVER);
	latest.sequence++;
	latest.present = result->present;
	latest.frame = result->frame_number;
	latest.uptime_ms = (uint32_t)k_uptime_get();
	have_latest = true;
	if (k_msgq_put(&changes, &latest, K_NO_WAIT) != 0) {
		/* Bound memory while offline; reconnect publishes the latest state. */
		(void)k_msgq_get(&changes, &discarded, K_NO_WAIT);
		(void)k_msgq_put(&changes, &latest, K_NO_WAIT);
		LOG_WRN("MQTT state queue full; oldest change discarded");
	}
	k_mutex_unlock(&state_lock);
	if (change_fd >= 0) {
		/* Wake the socket poll immediately without periodic queue polling. */
		(void)eventfd_write(change_fd, 1);
	}
}

static void mqtt_event(struct mqtt_client *c, const struct mqtt_evt *event)
{
	ARG_UNUSED(c);
	switch (event->type) {
	case MQTT_EVT_CONNACK:
		if (event->result != 0) {
			LOG_WRN("MQTT connection rejected: %d", event->result);
			failed = true;
		} else {
			connected = true;
			resync = true;
			LOG_INF("MQTT connected; topic %s", topic);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		connected = false;
		failed = true;
		break;
	case MQTT_EVT_PUBACK:
		if (event->result != 0) {
			failed = true;
		} else if (pending && event->param.puback.message_id == pending_id) {
			pending = false;
			LOG_INF("MQTT acknowledged present=%u sequence=%u%s",
				in_flight.present, in_flight.sequence,
				IS_ENABLED(CONFIG_RADAR_SIMULATE_PRESENCE) ? " (simulation)" : "");
		}
		break;
	default:
		break;
	}
}

static void queue_latest_after_connect(void)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	k_msgq_purge(&changes);
	if (have_latest) {
		(void)k_msgq_put(&changes, &latest, K_NO_WAIT);
	}
	k_mutex_unlock(&state_lock);
	resync = false;
}

static int publish_next(void)
{
	struct mqtt_publish_param params = {0};
	int len, rc;

	if (pending || k_msgq_get(&changes, &in_flight, K_NO_WAIT) != 0) {
		return 0;
	}
	len = snprintk(payload, sizeof(payload),
		"{\"present\":%u,\"state\":\"%s\",\"simulated\":%s,"
		"\"sequence\":%u,\"frame\":%u,\"uptime_ms\":%u}",
		in_flight.present, in_flight.present ? "human" : "nobody",
		IS_ENABLED(CONFIG_RADAR_SIMULATE_PRESENCE) ? "true" : "false",
		in_flight.sequence, in_flight.frame, in_flight.uptime_ms);
	if (len < 0 || (size_t)len >= sizeof(payload)) {
		return -EMSGSIZE;
	}
	if (++next_id == 0) {
		next_id = 1;
	}
	params.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	params.message.topic.topic.utf8 = (uint8_t *)topic;
	params.message.topic.topic.size = strlen(topic);
	params.message.payload.data = (uint8_t *)payload;
	params.message.payload.len = len;
	params.message_id = next_id;
	params.retain_flag = 1;
	rc = mqtt_publish(&client, &params);
	if (rc == 0) {
		pending_id = next_id;
		pending = true;
		ack_deadline = k_uptime_get() + 10000;
	}
	return rc;
}

static int open_connection(void)
{
	mqtt_client_init(&client);
	client.broker = &broker;
	client.evt_cb = mqtt_event;
	client.client_id.utf8 = (uint8_t *)client_id;
	client.client_id.size = strlen(client_id);
	client.user_name = &username;
	client.password = &password;
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;
	client.clean_session = 1;
	client.keepalive = CONFIG_RADAR_MQTT_KEEPALIVE_SECONDS;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);
	connected = failed = pending = resync = false;
	return mqtt_connect(&client);
}

static int mqtt_poll_timeout(int64_t connect_deadline)
{
	int64_t now = k_uptime_get();
	int timeout = connected ? mqtt_keepalive_time_left(&client) :
		(int)MAX(0, connect_deadline - now);

	if (pending) {
		timeout = MIN(timeout, (int)MAX(0, ack_deadline - now));
	}
	if (connected && (resync || (!pending && k_msgq_num_used_get(&changes) > 0))) {
		timeout = 0;
	}
	return timeout;
}

static void mqtt_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		while (!halow_sta_is_online()) {
			k_sleep(K_SECONDS(1));
		}
		int rc = open_connection();
		if (rc != 0) {
			LOG_WRN("MQTT connect failed: %d; retrying", rc);
			k_sleep(K_SECONDS(3));
			continue;
		}
		int64_t connect_deadline = k_uptime_get() + 5000;
		while (!failed && halow_sta_is_online()) {
			struct zsock_pollfd fds[2] = {
				{ .fd = client.transport.tcp.sock, .events = ZSOCK_POLLIN },
				{ .fd = change_fd, .events = ZSOCK_POLLIN },
			};
			rc = zsock_poll(fds, ARRAY_SIZE(fds), mqtt_poll_timeout(connect_deadline));
			if (rc < 0 || (fds[0].revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL))) {
				break;
			}
			if (fds[1].revents & ZSOCK_POLLIN) {
				eventfd_t value;
				(void)eventfd_read(change_fd, &value);
			}
			if (fds[0].revents & ZSOCK_POLLIN) {
				rc = mqtt_input(&client);
				if (rc != 0 && rc != -EAGAIN) {
					break;
				}
			}
			if (failed) {
				break;
			}
			if (!connected) {
				if (k_uptime_get() >= connect_deadline) {
					break;
				}
				continue;
			}
			if (resync) {
				queue_latest_after_connect();
			}
			rc = mqtt_live(&client);
			if (rc != 0 && rc != -EAGAIN) {
				break;
			}
			if (publish_next() != 0 || (pending && k_uptime_get() >= ack_deadline)) {
				break;
			}
		}
		(void)mqtt_abort(&client);
		connected = pending = false;
		LOG_WRN("MQTT disconnected; retrying with latest state");
		k_sleep(K_SECONDS(3));
	}
}

int radar_mqtt_start(void)
{
	uint8_t id[8];
	char hex[sizeof(id) * 2 + 1];
	ssize_t count = hwinfo_get_device_id(id, sizeof(id));

	if (count <= 0 || username.size == 0) {
		LOG_ERR("MQTT needs device ID and local broker credentials");
		return -EINVAL;
	}
	change_fd = eventfd(0, EFD_NONBLOCK);
	if (change_fd < 0) {
		return -errno;
	}
	for (int i = 0; i < count; i++) {
		snprintk(&hex[i * 2], 3, "%02x", id[i]);
	}
	snprintk(client_id, sizeof(client_id), "radar-%s", hex);
	int len = snprintk(topic, sizeof(topic), "%s/%s/presence",
			   CONFIG_RADAR_MQTT_TOPIC_PREFIX, hex);
	if (len < 0 || (size_t)len >= sizeof(topic)) {
		return -EMSGSIZE;
	}
	broker.sin_family = AF_INET;
	broker.sin_port = htons(CONFIG_RADAR_MQTT_PORT);
	if (net_addr_pton(AF_INET, CONFIG_RADAR_MQTT_HOST, &broker.sin_addr) != 0) {
		return -EINVAL;
	}
	LOG_INF("MQTT broker %s:%u; publishing changes to %s",
		CONFIG_RADAR_MQTT_HOST, CONFIG_RADAR_MQTT_PORT, topic);
	k_thread_create(&mqtt_thread, mqtt_stack, K_THREAD_STACK_SIZEOF(mqtt_stack),
			mqtt_thread_fn, NULL, NULL, NULL, 8, 0, K_NO_WAIT);
	k_thread_name_set(&mqtt_thread, "radar_mqtt");
	return 0;
}

#else
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(radar_mqtt, CONFIG_RADAR_LOG_LEVEL);

int radar_mqtt_start(void)
{
	LOG_INF("MQTT disabled: HaLow DTIM power comparison; no MQTT thread or keepalive");
	return 0;
}

void radar_mqtt_update(const struct iwrl_human_result *result)
{
	ARG_UNUSED(result);
}
#endif

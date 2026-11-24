/* LwM2M over WPA3 HaLow, using either UDP or the MAC transport. */
#ifdef RADAR_HALOW_LIGHT
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/posix/sys/eventfd.h>
#include <mmwlan.h>
#include "liblwm2m.h"
#include "halow_light.h"
#include "halow_sleep_diag.h"
#ifdef RADAR_HALOW_CAMERA
#include "halow_camera.h"
#endif

LOG_MODULE_REGISTER(halow_light, CONFIG_RADAR_LOG_LEVEL);
#define COAP_PORT 5683
#define HALOW_ETHERTYPE 0x88b5
#define FRAME_HEADER 17
#define FRAME_VERSION 0xc1
#define MAX_COAP 1024
#define LIGHT_RESOURCE 5850
#define DIGITAL_INPUT_STATE 5500
#define BUTTON_COUNTER 5501
#define DIAG_TEXT 5750 /* Push Button "Application Type" carries the idle report. */

extern lwm2m_object_t *get_security_object(int id, const char *uri, char *identity,
                                         char *key, uint16_t size, bool bootstrap);
extern void clean_security_object(lwm2m_object_t *object);
extern lwm2m_object_t *get_server_object(int id, const char *binding, int lifetime, bool storing);
extern lwm2m_object_t *get_object_device(void);

/* Required by the shared Device example. This profile disables its Execute handler. */
int g_reboot;

static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), presence_test_led_gpios);
static lwm2m_uri_t light_uri, button_state_uri, button_counter_uri;
static lwm2m_list_t instance = {.id = 0};
static bool light_on;
/* Bit zero holds the stable GPIO state. Upper bits count press edges. */
static atomic_t button_events;
#if !defined(CONFIG_RADAR_SIMULATE_PRESENCE)
/* Low bit carries presence. Upper bits carry range in centimetres. */
static atomic_t radar_sample;
static lwm2m_uri_t radar_uri, distance_uri;
static bool radar_present;
static uint32_t radar_distance_cm;
#endif
static bool button_pressed;
static uint32_t button_presses;
static atomic_t link_generation; /* odd when associated */
static int wake_fd = -1;
K_THREAD_STACK_DEFINE(light_stack, 6144);
static struct k_thread light_thread;

struct link_session {
	int fd;
	uint8_t mac[6];
	char uri[sizeof("coap://255.255.255.255:5683")];
#ifndef RADAR_HALOW_COAP_UDP
	uint8_t ap[6];
	struct sockaddr_ll address;
#endif
};

static int set_light(bool value)
{
	int rc = gpio_pin_set_dt(&led, value);
	if (rc == 0) {
		light_on = value;
		LOG_INF("3311/0/5850 %s", light_on ? "ON" : "OFF");
	}
	return rc;
}

static uint8_t light_read(lwm2m_context_t *ctx, uint16_t id, int *size,
			  lwm2m_data_t **data, lwm2m_object_t *object)
{
	if (id != 0) return COAP_404_NOT_FOUND;
	if (*size == 0) {
		*data = lwm2m_data_new(1);
		if (*data == NULL) return COAP_500_INTERNAL_SERVER_ERROR;
		*size = 1;
		(*data)[0].id = LIGHT_RESOURCE;
	}
	if (*size != 1 || (*data)[0].id != LIGHT_RESOURCE) return COAP_404_NOT_FOUND;
	lwm2m_data_encode_bool(light_on, *data);
	return COAP_205_CONTENT;
}

static uint8_t light_write(lwm2m_context_t *ctx, uint16_t id, int size,
			   lwm2m_data_t *data, lwm2m_object_t *object,
			   lwm2m_write_type_t type)
{
	bool value;
	if (id != 0 || size != 1 || data[0].id != LIGHT_RESOURCE) return COAP_404_NOT_FOUND;
	if (!lwm2m_data_decode_bool(data, &value)) return COAP_400_BAD_REQUEST;
	if (value != light_on) {
		if (set_light(value) != 0) return COAP_500_INTERNAL_SERVER_ERROR;
		lwm2m_resource_value_changed(ctx, &light_uri);
	}
	return COAP_204_CHANGED;
}

static lwm2m_object_t light = {
	.objID = 3311, .instanceList = &instance, .readFunc = light_read, .writeFunc = light_write,
};

#if defined(CONFIG_RADAR_HALOW_DIAG)
#include <hal/nrf_gpio.h>

/* Idle diagnostics: a 20 ms timer samples the three Morse control lines so the
 * share of time the radio is held or reports awake can be read over LwM2M.
 * WAKE is P1.05 (host output), IRQ is P1.06 (active low), BUSY is P1.10. */
#define DIAG_PIN_WAKE NRF_GPIO_PIN_MAP(1, 5)
#define DIAG_PIN_IRQ  NRF_GPIO_PIN_MAP(1, 6)
#define DIAG_PIN_BUSY NRF_GPIO_PIN_MAP(1, 10)

static struct {
	uint32_t samples, wake_high, busy_high, irq_low;
	uint32_t rx_frames, rx_group;
	int64_t since_ms;
} diag;

/* Keep the latest SDK-owned debugger snapshot per core. The SDK
 * returns TLVs after stripping the command response header. */
static struct mmwlan_morse_stats *volatile halow_diag_apps, *volatile halow_diag_mac;
static uint32_t halow_diag_apps_len, halow_diag_mac_len;
static struct k_spinlock diag_capture_lock;
static uint32_t diag_apps_samples, diag_apps_errors;
static bool diag_mac_attempted;

static uint32_t diag_payload_length(const struct mmwlan_morse_stats *stats)
{
	/* SDK 2.14.3 advances buf past the 16-byte header/status, but len
	 * still includes the four-byte status in the 1600-byte response. */
	if (stats == NULL || stats->len < 4 || stats->len > 1588) return 0;
	return stats->len - 4;
}

static void diag_capture(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(diag_capture_work, diag_capture);

static void diag_capture(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!(atomic_get(&link_generation) & 1)) return;

	struct mmwlan_morse_stats *apps = mmwlan_get_morse_stats(0, false);
	uint32_t apps_len = diag_payload_length(apps);
	struct mmwlan_morse_stats *mac = NULL;
	bool capture_mac = !diag_mac_attempted;

	if (capture_mac) mac = mmwlan_get_morse_stats(1, false);

	k_spinlock_key_t key = k_spin_lock(&diag_capture_lock);
	if (apps_len > 0) {
		struct mmwlan_morse_stats *previous = halow_diag_apps;

		halow_diag_apps = apps;
		halow_diag_apps_len = apps_len;
		diag_apps_samples++;
		apps = previous;
	} else {
		diag_apps_errors++;
	}
	if (capture_mac) {
		halow_diag_mac = mac;
		halow_diag_mac_len = diag_payload_length(mac);
		diag_mac_attempted = true;
	}
	k_spin_unlock(&diag_capture_lock, key);

	mmwlan_free_morse_stats(apps);
	if (atomic_get(&link_generation) & 1) {
		k_work_schedule(&diag_capture_work, K_SECONDS(CONFIG_RADAR_HALOW_DIAG_INTERVAL_S));
	}
}

static void diag_sample(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	diag.samples++;
	diag.wake_high += nrf_gpio_pin_out_read(DIAG_PIN_WAKE) ? 1U : 0U;
	diag.busy_high += nrf_gpio_pin_read(DIAG_PIN_BUSY) ? 1U : 0U;
	diag.irq_low += nrf_gpio_pin_read(DIAG_PIN_IRQ) ? 0U : 1U;
}

static K_TIMER_DEFINE(diag_timer, diag_sample, NULL);

/* Report the window since the previous read, then start a new window.
 * Shares are in hundredths of a percent. */
static void diag_report(char *text, size_t size)
{
	unsigned int key = irq_lock();
	uint32_t samples = diag.samples, wake = diag.wake_high;
	uint32_t busy = diag.busy_high, irq = diag.irq_low;
	uint32_t frames = diag.rx_frames, group = diag.rx_group;
	int64_t now = k_uptime_get();
	uint32_t seconds = (uint32_t)((now - diag.since_ms) / 1000);

	diag.samples = diag.wake_high = diag.busy_high = diag.irq_low = 0;
	diag.rx_frames = diag.rx_group = 0;
	diag.since_ms = now;
	irq_unlock(key);

	uint32_t divisor = samples > 0 ? samples : 1;
	k_spinlock_key_t capture_key = k_spin_lock(&diag_capture_lock);
	uint32_t apps_len = halow_diag_apps_len, mac_len = halow_diag_mac_len;
	uint32_t apps_samples = diag_apps_samples, apps_errors = diag_apps_errors;
	bool ready = diag_mac_attempted;
	k_spin_unlock(&diag_capture_lock, capture_key);
	snprintk(text, size, "t=%us wake=%u busy=%u irqlow=%u rx=%u group=%u stats=%d apps=%u mac=%u apps_samples=%u apps_errors=%u",
		 (unsigned int)seconds, (unsigned int)((uint64_t)wake * 10000U / divisor),
		 (unsigned int)((uint64_t)busy * 10000U / divisor),
		 (unsigned int)((uint64_t)irq * 10000U / divisor),
		 (unsigned int)frames, (unsigned int)group,
		 (int)ready,
		 apps_len, mac_len, apps_samples, apps_errors);
}
#endif

static uint8_t button_discover(lwm2m_context_t *ctx, uint16_t id, int *size,
			   lwm2m_data_t **data, lwm2m_object_t *object)
{
	if (id != 0) return COAP_404_NOT_FOUND;
	if (*size == 0) {
		int count = (IS_ENABLED(CONFIG_RADAR_HALOW_DIAG) ||
			     IS_ENABLED(CONFIG_RADAR_HALOW_SLEEP_DIAG)) ? 3 : 2;

		*data = lwm2m_data_new(count);
		if (*data == NULL) return COAP_500_INTERNAL_SERVER_ERROR;
		*size = count;
		(*data)[0].id = DIGITAL_INPUT_STATE;
		(*data)[1].id = BUTTON_COUNTER;
		if (count == 3) (*data)[2].id = DIAG_TEXT;
	}
	return COAP_205_CONTENT;
}

static uint8_t button_read(lwm2m_context_t *ctx, uint16_t id, int *size,
			   lwm2m_data_t **data, lwm2m_object_t *object)
{
	uint8_t status = button_discover(ctx, id, size, data, object);

	if (status != COAP_205_CONTENT) return status;
	for (int index = 0; index < *size; ++index) {
		lwm2m_data_t *resource = &(*data)[index];
		switch (resource->id) {
		case DIGITAL_INPUT_STATE:
			lwm2m_data_encode_bool(button_pressed, resource);
			break;
		case BUTTON_COUNTER:
			lwm2m_data_encode_int(button_presses, resource);
			break;
#if defined(CONFIG_RADAR_HALOW_SLEEP_DIAG)
		case DIAG_TEXT: {
			char text[HALOW_SLEEP_DIAG_TEXT_SIZE];

			halow_sleep_diag_report(text, sizeof(text));
			lwm2m_data_encode_string(text, resource);
			break;
		}
#elif defined(CONFIG_RADAR_HALOW_DIAG)
		case DIAG_TEXT: {
			char text[192];

			diag_report(text, sizeof(text));
			lwm2m_data_encode_string(text, resource);
			break;
		}
#endif
		default:
			return COAP_404_NOT_FOUND;
		}
	}
	return COAP_205_CONTENT;
}

static lwm2m_list_t button_instance = {.id = 0};
static lwm2m_object_t button = {
	.objID = 3347, .instanceList = &button_instance,
	.readFunc = button_read, .discoverFunc = button_discover,
};

#if !defined(CONFIG_RADAR_SIMULATE_PRESENCE)
static uint8_t radar_read(lwm2m_context_t *ctx, uint16_t id, int *size,
			  lwm2m_data_t **data, lwm2m_object_t *object)
{
	if (id != 0) return COAP_404_NOT_FOUND;
	if (*size == 0) {
		*data = lwm2m_data_new(1);
		if (*data == NULL) return COAP_500_INTERNAL_SERVER_ERROR;
		*size = 1;
		(*data)[0].id = DIGITAL_INPUT_STATE; /* Digital Input State (5500). */
	}
	for (int index = 0; index < *size; ++index) {
		if ((*data)[index].id != DIGITAL_INPUT_STATE) return COAP_404_NOT_FOUND;
		lwm2m_data_encode_bool(radar_present, &(*data)[index]);
	}
	return COAP_205_CONTENT;
}

static lwm2m_list_t radar_instance = {.id = 0};
static lwm2m_object_t radar_presence = {
	.objID = 3302, .instanceList = &radar_instance, .readFunc = radar_read,
};
static uint8_t distance_read(lwm2m_context_t *ctx, uint16_t id, int *size,
			     lwm2m_data_t **data, lwm2m_object_t *object)
{
	if (id != 0) return COAP_404_NOT_FOUND;
	if (*size == 0) {
		*data = lwm2m_data_new(2);
		if (*data == NULL) return COAP_500_INTERNAL_SERVER_ERROR;
		*size = 2;
		(*data)[0].id = 5700;
		(*data)[1].id = 5701;
	}
	for (int index = 0; index < *size; ++index) {
		lwm2m_data_t *resource = &(*data)[index];
		switch (resource->id) {
		case 5700:
			lwm2m_data_encode_float(radar_distance_cm / 100.0, resource);
			break;
		case 5701:
			lwm2m_data_encode_string("m", resource);
			break;
		default:
			return COAP_404_NOT_FOUND;
		}
	}
	return COAP_205_CONTENT;
}

static lwm2m_list_t distance_instance = {.id = 0};
static lwm2m_object_t radar_distance = {
	.objID = 3330, .instanceList = &distance_instance, .readFunc = distance_read,
};
#endif

static void process_button(lwm2m_context_t *ctx)
{
	uint32_t events = (uint32_t)atomic_get(&button_events);
	bool pressed = (events & 1) != 0;
	uint32_t presses = events >> 1;
	if (pressed != button_pressed) {
		button_pressed = pressed;
		if (ctx != NULL) lwm2m_resource_value_changed(ctx, &button_state_uri);
	}
	if (presses != button_presses) {
		bool toggle = ((presses ^ button_presses) & 1) != 0;
		button_presses = presses;
		if (ctx != NULL) lwm2m_resource_value_changed(ctx, &button_counter_uri);
		if (toggle && set_light(!light_on) == 0 && ctx != NULL) {
			lwm2m_resource_value_changed(ctx, &light_uri);
		}
	}
}

void *lwm2m_malloc(size_t size) { return malloc(size); }
void lwm2m_free(void *value)
{
#ifdef RADAR_HALOW_CAMERA
	halow_camera_free(value);
#else
	free(value);
#endif
}
char *lwm2m_strdup(const char *value)
{
	size_t length = strlen(value) + 1;
	char *copy = malloc(length);
	if (copy != NULL) memcpy(copy, value, length);
	return copy;
}
int lwm2m_strncmp(const char *a, const char *b, size_t size) { return strncmp(a, b, size); }
int lwm2m_strcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }
time_t lwm2m_gettime(void) { return k_uptime_get() / 1000; }
int lwm2m_seed(void) { return (int)sys_rand32_get(); }
void lwm2m_printf(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vprintk(format, args);
	va_end(args);
}

void *lwm2m_connect_server(uint16_t id, void *data) { return data; }
void lwm2m_close_connection(void *session, void *data) {}
void lwm2m_session_remove(void *session) {}
bool lwm2m_session_is_equal(void *a, void *b, void *data) { return a == b; }

uint8_t lwm2m_buffer_send(void *handle, uint8_t *data, size_t length, void *arg)
{
	struct link_session *link = handle;
	if (link->fd < 0 || length > MAX_COAP) return COAP_500_INTERNAL_SERVER_ERROR;
	halow_sleep_diag_activity();
#ifdef RADAR_HALOW_COAP_UDP
	return zsock_send(link->fd, data, length, 0) == length ?
		COAP_NO_ERROR : COAP_500_INTERNAL_SERVER_ERROR;
#else
	uint8_t frame[FRAME_HEADER + MAX_COAP];
	memcpy(frame, link->ap, 6);
	memcpy(frame + 6, link->mac, 6);
	sys_put_be16(HALOW_ETHERTYPE, frame + 12);
	frame[14] = FRAME_VERSION;
	sys_put_be16(length, frame + 15);
	memcpy(frame + FRAME_HEADER, data, length);
	return zsock_sendto(link->fd, frame, FRAME_HEADER + length, 0,
			   (struct sockaddr *)&link->address, sizeof(link->address)) ==
		FRAME_HEADER + length ? COAP_NO_ERROR : COAP_500_INTERNAL_SERVER_ERROR;
#endif
}

#ifdef RADAR_HALOW_COAP_UDP
static int open_link(struct link_session *link)
{
	struct net_if *iface = net_if_get_first_wifi();
	if (iface == NULL || mmwlan_get_mac_addr(link->mac) != MMWLAN_SUCCESS) {
		return -ENETDOWN;
	}
	struct net_in_addr *local_ip = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (local_ip == NULL) {
		return -EADDRNOTAVAIL;
	}
	struct sockaddr_in local = {
		.sin_family = AF_INET, .sin_addr = *local_ip,
	};
	struct sockaddr_in server = {
		.sin_family = AF_INET, .sin_port = htons(COAP_PORT),
		.sin_addr = iface->config.dhcpv4.server_id,
	};
	if (CONFIG_RADAR_HALOW_COAP_SERVER[0] != '\0' &&
	    net_addr_pton(AF_INET, CONFIG_RADAR_HALOW_COAP_SERVER, &server.sin_addr) != 0) {
		return -EINVAL;
	}
	if (server.sin_addr.s_addr == 0) {
		return -EDESTADDRREQ;
	}
	char address[NET_IPV4_ADDR_LEN];
	if (net_addr_ntop(AF_INET, &server.sin_addr, address, sizeof(address)) == NULL) {
		return -EINVAL;
	}
	snprintk(link->uri, sizeof(link->uri), "coap://%s:%u", address, COAP_PORT);

	int fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		return -errno;
	}
	if (zsock_bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0 ||
	    zsock_connect(fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
		int error = errno;
		zsock_close(fd);
		return -error;
	}
	return fd;
}

static void receive(lwm2m_context_t *ctx, struct link_session *link)
{
	uint8_t datagram[MAX_COAP + 1];
	ssize_t length = zsock_recv(link->fd, datagram, sizeof(datagram), ZSOCK_MSG_DONTWAIT);
	if (length >= 4 && length <= MAX_COAP) {
		halow_sleep_diag_activity();
		lwm2m_handle_packet(ctx, datagram, length, link);
	}
}
#else
static int open_link(struct link_session *link)
{
	struct net_if *iface = net_if_get_first_wifi();
	if (iface == NULL || mmwlan_get_bssid(link->ap) != MMWLAN_SUCCESS ||
	    mmwlan_get_mac_addr(link->mac) != MMWLAN_SUCCESS) return -ENETDOWN;

	strcpy(link->uri, "coap://127.0.0.1:5683");
	link->address = (struct sockaddr_ll){
		.sll_family = AF_PACKET, .sll_ifindex = net_if_get_by_iface(iface),
		.sll_protocol = sys_cpu_to_be16(ETH_P_ALL), .sll_halen = 6,
	};
	memcpy(link->address.sll_addr, link->ap, 6);
	int fd = zsock_socket(AF_PACKET, SOCK_RAW, sys_cpu_to_be16(ETH_P_ALL));
	if (fd < 0) return -errno;
	if (zsock_bind(fd, (struct sockaddr *)&link->address, sizeof(link->address)) < 0) {
		int error = errno;
		zsock_close(fd);
		return -error;
	}
	return fd;
}

static void receive(lwm2m_context_t *ctx, struct link_session *link)
{
	uint8_t frame[FRAME_HEADER + MAX_COAP + 1];
	ssize_t n = zsock_recv(link->fd, frame, sizeof(frame), ZSOCK_MSG_DONTWAIT);
#if defined(CONFIG_RADAR_HALOW_DIAG)
	if (n >= 6) {
		diag.rx_frames++;
		diag.rx_group += (frame[0] & 1U) ? 1U : 0U;
	}
#endif
	if (n < FRAME_HEADER || memcmp(frame, link->mac, 6) != 0 ||
	    memcmp(frame + 6, link->ap, 6) != 0 || sys_get_be16(frame + 12) != HALOW_ETHERTYPE ||
	    frame[14] != FRAME_VERSION) return;
	size_t length = sys_get_be16(frame + 15);
	if (length >= 4 && length <= MAX_COAP && FRAME_HEADER + length <= n) {
		halow_sleep_diag_activity();
		lwm2m_handle_packet(ctx, frame + FRAME_HEADER, length, link);
	}
}

#endif

static void run(void *unused1, void *unused2, void *unused3)
{
	struct link_session link = {.fd = -1};
	lwm2m_context_t *ctx = NULL;
	lwm2m_object_t *objects[] = {
		NULL,
		get_server_object(1, "U", 300, false), get_object_device(), &light, &button,
#ifdef RADAR_HALOW_CAMERA
		&halow_camera,
#endif
#if !defined(CONFIG_RADAR_SIMULATE_PRESENCE)
		&radar_presence, &radar_distance,
#endif
	};
	if (!objects[1] || !objects[2]) {
		LOG_ERR("Could not allocate Light Control LwM2M objects");
		return;
	}
	objects[2]->executeFunc = NULL;
	lwm2m_stringToUri("/3311/0/5850", sizeof("/3311/0/5850") - 1, &light_uri);
	lwm2m_stringToUri("/3347/0/5500", sizeof("/3347/0/5500") - 1, &button_state_uri);
	lwm2m_stringToUri("/3347/0/5501", sizeof("/3347/0/5501") - 1, &button_counter_uri);
#if !defined(CONFIG_RADAR_SIMULATE_PRESENCE)
	lwm2m_stringToUri("/3302/0/5500", sizeof("/3302/0/5500") - 1, &radar_uri);
	lwm2m_stringToUri("/3330/0/5700", sizeof("/3330/0/5700") - 1, &distance_uri);
#endif
	atomic_val_t active = 0;
	for (;;) {
		atomic_val_t generation = atomic_get(&link_generation);
		if (generation != active) {
			if (link.fd >= 0) zsock_close(link.fd);
			link.fd = -1;
			if (ctx != NULL) lwm2m_close(ctx);
			ctx = NULL;
			if (objects[0] != NULL) {
				clean_security_object(objects[0]);
				lwm2m_free(objects[0]);
				objects[0] = NULL;
			}
			active = generation;
		}
		if ((active & 1) && ctx == NULL) {
			link.fd = open_link(&link);
			if (link.fd >= 0) {
				char endpoint[19];
				snprintk(endpoint, sizeof(endpoint), "halow-%02x%02x%02x%02x%02x%02x",
					link.mac[0], link.mac[1], link.mac[2], link.mac[3], link.mac[4], link.mac[5]);
				objects[0] = get_security_object(1, link.uri, NULL, NULL, 0, false);
				ctx = objects[0] != NULL ? lwm2m_init(&link) : NULL;
				if (ctx != NULL && lwm2m_configure(ctx, endpoint, NULL, NULL,
								   ARRAY_SIZE(objects), objects) == 0) {
					LOG_INF("LwM2M endpoint %s via %s", endpoint, link.uri);
				} else {
					if (ctx != NULL) lwm2m_close(ctx);
					ctx = NULL;
					if (objects[0] != NULL) {
						clean_security_object(objects[0]);
						lwm2m_free(objects[0]);
						objects[0] = NULL;
					}
					zsock_close(link.fd);
					link.fd = -1;
				}
			}
		}
#if !defined(CONFIG_RADAR_SIMULATE_PRESENCE)
		uint32_t sample = (uint32_t)atomic_get(&radar_sample);
		bool present = (sample & 1U) != 0;
		uint32_t distance_cm = sample >> 1;
		if (radar_present != present) {
			radar_present = present;
			if (ctx != NULL) lwm2m_resource_value_changed(ctx, &radar_uri);
		}
		if (radar_distance_cm != distance_cm) {
			radar_distance_cm = distance_cm;
			if (ctx != NULL) lwm2m_resource_value_changed(ctx, &distance_uri);
		}
#endif
		process_button(ctx);

		time_t timeout = 300;
		int wait_ms = (active & 1) ? 1000 : -1;
		if (ctx != NULL && lwm2m_step(ctx, &timeout) == 0) {
			wait_ms = CLAMP(timeout, 0, 300) * 1000;
		}
		struct zsock_pollfd fds[] = {
			{.fd = wake_fd, .events = ZSOCK_POLLIN},
			{.fd = link.fd, .events = ZSOCK_POLLIN},
		};
		if (zsock_poll(fds, ARRAY_SIZE(fds), wait_ms) > 0) {
			if (fds[0].revents & ZSOCK_POLLIN) {
				eventfd_t ticks;
				eventfd_read(wake_fd, &ticks);
			}
			if ((fds[1].revents & ZSOCK_POLLIN) && active == atomic_get(&link_generation)) {
				receive(ctx, &link);
			}
		}
	}
}

void halow_light_button_changed(bool pressed)
{
	uint32_t events = (uint32_t)atomic_get(&button_events);
	/* The existing debounce work item serializes pin transitions. */
	if ((events & 1) == pressed) return;
	atomic_set(&button_events, (atomic_val_t)((events & ~1U) + (pressed ? 3U : 0U)));
	if (wake_fd >= 0) eventfd_write(wake_fd, 1);
}

#if !defined(CONFIG_RADAR_SIMULATE_PRESENCE)
void halow_light_radar_sample(bool present, float range_m)
{
	uint32_t distance_cm = 0;

	if (present && range_m > 0.0f && range_m <= 10000.0f) {
		distance_cm = (uint32_t)(range_m * 100.0f + 0.5f);
	}
	atomic_val_t sample = (atomic_val_t)((distance_cm << 1) | (present ? 1U : 0U));

	if (atomic_set(&radar_sample, sample) != sample && wake_fd >= 0) {
		eventfd_write(wake_fd, 1);
	}
}

#endif

void halow_light_link_changed(bool connected)
{
	if ((atomic_get(&link_generation) & 1) != connected) {
		atomic_inc(&link_generation);
		halow_sleep_diag_link_changed(connected);
#if defined(CONFIG_RADAR_HALOW_DIAG)
		if (connected) {
			k_work_reschedule(&diag_capture_work, K_SECONDS(15));
		} else {
			k_work_cancel_delayable(&diag_capture_work);
		}
#endif
		if (wake_fd >= 0) eventfd_write(wake_fd, 1);
	}
}

int halow_light_start(bool initial_pressed)
{
	button_pressed = initial_pressed;
	atomic_set(&button_events, initial_pressed);
	wake_fd = eventfd(0, EFD_NONBLOCK);
	if (wake_fd < 0) return -errno;
#if defined(CONFIG_RADAR_HALOW_DIAG)
	diag.since_ms = k_uptime_get();
	k_timer_start(&diag_timer, K_MSEC(20), K_MSEC(20));
#endif
	k_thread_create(&light_thread, light_stack, K_THREAD_STACK_SIZEOF(light_stack),
			run, NULL, NULL, NULL, CONFIG_RADAR_THREAD_PRIORITY, 0, K_NO_WAIT);
	return 0;
}
#endif /* RADAR_HALOW_LIGHT */

/* VC0706 capture and legacy LwM2M Camera object 5001 over HaLow. */
#ifdef RADAR_HALOW_CAMERA
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>

#include "halow_camera.h"

LOG_MODULE_REGISTER(halow_camera, CONFIG_RADAR_LOG_LEVEL);

#define CAPTURE 0
#define IMAGE 1
#define IMAGE_LENGTH 2
#define RESOLUTION 3
#define BUFFER_SIZE 5
#define JPEG_CAPACITY (64U * 1024U)
#define JPEG_TLV_HEADER_SHORT 4U
#define JPEG_TLV_HEADER_MAX 5U
#define CAMERA_CROP_SOURCE_WIDTH 1024U
#define CAMERA_CROP_SOURCE_HEIGHT 768U

static const struct device *const uart = DEVICE_DT_GET(DT_ALIAS(camera0));
static const struct gpio_dt_spec power =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), camera_power_gpios);
PINCTRL_DT_DEFINE(DT_ALIAS(camera0));
static const struct pinctrl_dev_config *const camera_pins =
	PINCTRL_DT_DEV_CONFIG_GET(DT_ALIAS(camera0));
RING_BUF_DECLARE(rx_ring, 1024);
static K_SEM_DEFINE(rx_ready, 0, 1);
/* Reserve a resource TLV header before the JPEG so Block2 can borrow it. */
static uint8_t jpeg_storage[JPEG_TLV_HEADER_MAX + JPEG_CAPACITY];
static uint8_t *const jpeg = &jpeg_storage[JPEG_TLV_HEADER_MAX];
static size_t jpeg_length;
struct camera_resolution {
	const char *name;
	uint16_t width;
	uint16_t height;
	uint8_t field;
	uint8_t code;
	bool crop;
};
static const struct camera_resolution camera_sizes[] = {
	{"640x480", 640, 480, 0x04, 0x00, false},
	{"320x240", 320, 240, 0x04, 0x11, false},
	{"160x120", 160, 120, 0x04, 0x22, false},
	{"1024x768", 1024, 768, 0x05, 0x33, false},
	{"800x600", 800, 600, 0x05, 0x33, true},
	{"1024x720", 1024, 720, 0x05, 0x33, true},
};
static size_t camera_size;
static bool ready;
static bool uart_configured;

static void uart_rx(const struct device *dev, void *user)
{
	uint8_t data[32];

	ARG_UNUSED(user);
	if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) return;
	for (;;) {
		int count = uart_fifo_read(dev, data, sizeof(data));

		if (count <= 0) break;
		ring_buf_put(&rx_ring, data, count);
		k_sem_give(&rx_ready);
	}
}

/* With deferred init, no UART driver API may run before device_init(). */
static int camera_uart_init(void)
{
	if (uart_configured) return 0;
#if DT_NODE_HAS_PROP(DT_ALIAS(camera0), zephyr_deferred_init)
	if (!device_is_ready(uart)) {
		int rc = device_init(uart);

		if (rc != 0) return rc;
	}
#endif
	if (!device_is_ready(uart)) return -ENODEV;
	int rc = uart_irq_callback_user_data_set(uart, uart_rx, NULL);

	if (rc == 0) uart_configured = true;
	return rc;
}

static int camera_standby(void)
{
	int rc = 0;

	/* Before the first deferred capture, do not initialize or touch the UART
	 * driver just to suspend it. Only park its pins and camera supply. */
	if (device_is_ready(uart)) {
		uart_irq_rx_disable(uart);
		uart_irq_tx_disable(uart);
		uart_irq_err_disable(uart);
		rc = pm_device_action_run(uart, PM_DEVICE_ACTION_SUSPEND);
	}
	if (rc == 0 || rc == -EALREADY) {
		/* An already-suspended device skips its PM callback. Reapply sleep
		 * pinctrl so TX/RX buffers, pulls and pin clocks are parked anyway. */
		rc = pinctrl_apply_state(camera_pins, PINCTRL_STATE_SLEEP);
	}
	int power_rc = gpio_pin_set_dt(&power, 0);

	ring_buf_reset(&rx_ring);
	k_sem_reset(&rx_ready);
	return rc != 0 ? rc : power_rc;
}

int halow_camera_start(void)
{
	if (!gpio_is_ready_dt(&power)) return -ENODEV;
	int rc = gpio_pin_configure_dt(&power, GPIO_OUTPUT_INACTIVE);

	if (rc != 0) return rc;
#if !DT_NODE_HAS_PROP(DT_ALIAS(camera0), zephyr_deferred_init)
	/* Retain the validated eager-init path in the original baseline env. */
	rc = camera_uart_init();
	if (rc != 0) return rc;
#endif
	rc = camera_standby();
	if (rc != 0) return rc;
	ready = true;
	return 0;
}

static void send_command(const uint8_t *data, size_t length)
{
	uart_irq_rx_disable(uart);
	ring_buf_reset(&rx_ring);
	k_sem_reset(&rx_ready);
	uart_irq_rx_enable(uart);
	for (size_t i = 0; i < length; ++i) uart_poll_out(uart, data[i]);
}

static int read_exact(uint8_t *data, size_t length, int64_t deadline)
{
	while (length) {
		size_t count = ring_buf_get(&rx_ring, data, length);

		if (count) {
			data += count;
			length -= count;
			continue;
		}
		int64_t remaining = deadline - k_uptime_get();

		if (remaining <= 0 || k_sem_take(&rx_ready, K_MSEC(remaining)) != 0) {
			return -ETIMEDOUT;
		}
	}
	return 0;
}

static int read_ack(uint8_t command, uint8_t length, int64_t deadline)
{
	uint8_t header[5];
	const uint8_t expected[] = {0x76, 0, command, 0, length};
	int rc = read_exact(header, sizeof(header), deadline);

	if (rc != 0) {
		LOG_WRN("Camera command 0x%02x response timeout", command);
		return rc;
	}
	/* The tracker-compatible camera returns status 1 for the resolution
	 * write. Accept that variant only for the zero-length 0x31 reply. */
	if (command == 0x31 && length == 0 && header[3] == 1) header[3] = 0;
	if (memcmp(header, expected, sizeof(header)) != 0) {
		LOG_HEXDUMP_WRN(header, sizeof(header), "Unexpected camera response");
		LOG_WRN("Expected camera command 0x%02x response length %u", command, length);
		return -EBADMSG;
	}
	return 0;
}

static int command_ack(const uint8_t *command, size_t length, int64_t deadline)
{
	send_command(command, length);
	return read_ack(command[2], 0, deadline);
}

static int camera_set_crop(const struct camera_resolution *mode, int64_t deadline)
{
	/* Putal V1.14 §1.20. Both center crops meet the X/Y alignment requirements. */
	uint8_t command[14] = {0x56, 0, 0x8b, 0x0a, 1, 1};
	const uint8_t expected[] = {0x76, 0, 0x8b, 2, 1, 0};
	uint8_t reply[sizeof(expected)];

	sys_put_be16((CAMERA_CROP_SOURCE_WIDTH - mode->width) / 2, &command[6]);
	sys_put_be16((CAMERA_CROP_SOURCE_HEIGHT - mode->height) / 2, &command[8]);
	sys_put_be16(mode->width, &command[10]);
	sys_put_be16(mode->height, &command[12]);
	send_command(command, sizeof(command));
	int rc = read_exact(reply, sizeof(reply), deadline);

	if (rc != 0) return rc;
	if (memcmp(reply, expected, sizeof(reply)) != 0) {
		LOG_HEXDUMP_WRN(reply, sizeof(reply), "Camera crop rejected");
		return -EBADMSG;
	}
	return 0;
}

/* Check the actual image size because some modules acknowledge an unsupported mode. */
static bool jpeg_matches_resolution(size_t length, const struct camera_resolution *mode)
{
	size_t offset = 2; /* Skip the SOI marker, checked by the caller. */

	while (length - offset >= 2 && jpeg[offset++] == 0xff) {
		while (offset < length && jpeg[offset] == 0xff) offset++;
		if (offset == length) break;
		uint8_t marker = jpeg[offset++];

		if (marker == 0xda || marker == 0xd9 || length - offset < 2) break;
		uint16_t segment_length = sys_get_be16(&jpeg[offset]);

		if (segment_length < 2 || segment_length > length - offset) break;
		/* Start Of Frame markers contain the dimensions before the scan data. */
		if (marker >= 0xc0 && marker <= 0xcf &&
		    marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
			return segment_length >= 8 &&
			       sys_get_be16(&jpeg[offset + 3]) == mode->height &&
			       sys_get_be16(&jpeg[offset + 5]) == mode->width;
		}
		offset += segment_length;
	}
	return false;
}

static int camera_capture(void)
{
	const struct camera_resolution *mode = &camera_sizes[camera_size];
	const uint8_t resolution[] = {0x56, 0, 0x31, 5, mode->field, 1, 0, 0x19, mode->code};
	const uint8_t release[] = {0x56, 0, 0x36, 1, 3};
	const uint8_t freeze[] = {0x56, 0, 0x36, 1, 0};
	const uint8_t length_command[] = {0x56, 0, 0x34, 1, 0};
	uint8_t chunk_command[] = {0x56, 0, 0x32, 0x0c, 0, 0x0a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff};
	uint8_t encoded_length[4];
	uint32_t length = 0;

	int64_t deadline = k_uptime_get() + 5000;
	bool extended = mode->field == 0x05;
	int rc = 0;

	if (extended) rc = command_ack(release, sizeof(release), deadline);
	if (rc == 0) rc = command_ack(resolution, sizeof(resolution), deadline);

	if (rc != 0) return rc;
	if (extended) k_msleep(250);
	/* Changing the base resolution clears the camera's previous crop. */
	if (mode->crop) {
		rc = camera_set_crop(mode, deadline);
		if (rc != 0) return rc;
	}
	rc = command_ack(freeze, sizeof(freeze), deadline);
	if (rc == 0) {
		send_command(length_command, sizeof(length_command));
		rc = read_ack(0x34, sizeof(encoded_length), deadline);
	}
	if (rc == 0) rc = read_exact(encoded_length, sizeof(encoded_length), deadline);
	if (rc == 0) {
		length = sys_get_be32(encoded_length);
		if (length < 4 || length > JPEG_CAPACITY) {
			LOG_WRN("Camera reported unsupported JPEG size %u", (unsigned int)length);
			rc = -EMSGSIZE;
		}
	}
	if (rc == 0) jpeg_length = 0;
	for (uint32_t offset = 0; rc == 0 && offset < length;) {
		uint32_t chunk = MIN(256U, length - offset);

		sys_put_be32(offset, &chunk_command[6]);
		sys_put_be32(chunk, &chunk_command[10]);
		send_command(chunk_command, sizeof(chunk_command));
		rc = read_ack(0x32, 0, deadline);
		if (rc == 0) rc = read_exact(&jpeg[offset], chunk, deadline);
		if (rc == 0) rc = read_ack(0x32, 0, deadline);
		offset += chunk;
	}

	if (rc != 0) return rc;
	if (sys_get_be16(jpeg) != 0xffd8 || sys_get_be16(&jpeg[length - 2]) != 0xffd9) {
		LOG_WRN("Invalid JPEG markers at length %u: %04x, %04x", (unsigned int)length,
			sys_get_be16(jpeg), sys_get_be16(&jpeg[length - 2]));
		return -EBADMSG;
	}
	if (extended && !jpeg_matches_resolution(length, mode)) {
		LOG_WRN("Camera returned a JPEG that does not match %s", mode->name);
		return -EBADMSG;
	}
	jpeg_length = length;
	return 0;
}

static int camera_capture_powered(void)
{
	if (!ready) return -EAGAIN;
	int rc = gpio_pin_set_dt(&power, 1);

	if (rc != 0) return rc;
	k_sleep(K_SECONDS(4));
	rc = camera_uart_init();
	if (rc == 0) rc = pm_device_action_run(uart, PM_DEVICE_ACTION_RESUME);
	if (rc == 0 || rc == -EALREADY) {
		rc = camera_capture();
	}
	int standby_rc = camera_standby();

	if (standby_rc != 0) {
		LOG_ERR("Camera standby failed: %d", standby_rc);
		if (rc == 0) rc = standby_rc;
	}
	return rc;
}

static uint8_t camera_read(lwm2m_context_t *ctx, uint16_t id, int *size,
			   lwm2m_data_t **data, lwm2m_object_t *object)
{
	if (id != 0) return COAP_404_NOT_FOUND;
	if (*size == 0) {
		*data = lwm2m_data_new(3);
		if (*data == NULL) return COAP_500_INTERNAL_SERVER_ERROR;
		*size = 3;
		(*data)[0].id = IMAGE_LENGTH;
		(*data)[1].id = RESOLUTION;
		(*data)[2].id = BUFFER_SIZE;
	}
	for (int i = 0; i < *size; ++i) {
		lwm2m_data_t *entry = &(*data)[i];

		switch (entry->id) {
		case IMAGE:
			if (!jpeg_length) return COAP_404_NOT_FOUND;
			entry->type = LWM2M_TYPE_OPAQUE;
			entry->value.asBuffer.buffer = jpeg;
			entry->value.asBuffer.length = jpeg_length;
			break;
		case IMAGE_LENGTH:
			lwm2m_data_encode_int(jpeg_length, entry);
			break;
		case RESOLUTION:
			lwm2m_data_encode_string(camera_sizes[camera_size].name, entry);
			break;
		case BUFFER_SIZE:
			lwm2m_data_encode_int(JPEG_CAPACITY, entry);
			break;
		default:
			return COAP_404_NOT_FOUND;
		}
	}
	return COAP_205_CONTENT;
}

static uint8_t camera_discover(lwm2m_context_t *ctx, uint16_t id, int *size,
			       lwm2m_data_t **data, lwm2m_object_t *object)
{
	static const uint16_t resources[] = {CAPTURE, IMAGE, IMAGE_LENGTH, RESOLUTION, BUFFER_SIZE};

	if (id != 0) return COAP_404_NOT_FOUND;
	if (*size == 0) {
		*data = lwm2m_data_new(ARRAY_SIZE(resources));
		if (*data == NULL) return COAP_500_INTERNAL_SERVER_ERROR;
		*size = ARRAY_SIZE(resources);
		for (int i = 0; i < *size; ++i) (*data)[i].id = resources[i];
	}
	return COAP_205_CONTENT;
}

static uint8_t camera_write(lwm2m_context_t *ctx, uint16_t id, int count,
			    lwm2m_data_t *data, lwm2m_object_t *object,
			    lwm2m_write_type_t type)
{
	if (id != 0) return COAP_404_NOT_FOUND;
	if (count != 1 || data[0].id != RESOLUTION) return COAP_405_METHOD_NOT_ALLOWED;
	if (data[0].type != LWM2M_TYPE_STRING && data[0].type != LWM2M_TYPE_OPAQUE) {
		return COAP_400_BAD_REQUEST;
	}
	const uint8_t *value = data[0].value.asBuffer.buffer;

	for (size_t i = 0; i < ARRAY_SIZE(camera_sizes); ++i) {
		size_t length = strlen(camera_sizes[i].name);

		if (data[0].value.asBuffer.length == length &&
		    memcmp(value, camera_sizes[i].name, length) == 0) {
			camera_size = i;
			return COAP_204_CHANGED;
		}
	}
	return COAP_400_BAD_REQUEST;
}

static void camera_notify(lwm2m_context_t *ctx, const char *path)
{
	lwm2m_uri_t uri;

	lwm2m_stringToUri(path, strlen(path), &uri);
	lwm2m_resource_value_changed(ctx, &uri);
}

static uint8_t camera_execute(lwm2m_context_t *ctx, uint16_t id, uint16_t resource,
			      uint8_t *args, int length, lwm2m_object_t *object)
{
	if (id != 0) return COAP_404_NOT_FOUND;
	if (resource != CAPTURE) return COAP_405_METHOD_NOT_ALLOWED;
	if (length != 0) return COAP_400_BAD_REQUEST;
	int rc = camera_capture_powered();

	if (rc != 0) {
		LOG_WRN("Camera capture failed: %d", rc);
		if (rc == -EAGAIN) return COAP_503_SERVICE_UNAVAILABLE;
		return COAP_500_INTERNAL_SERVER_ERROR;
	}
	camera_notify(ctx, "/5001/0/2");
	return COAP_204_CHANGED;
}

static lwm2m_list_t camera_instance = {.id = 0};
lwm2m_object_t halow_camera = {
	.objID = 5001,
	.instanceList = &camera_instance,
	.readFunc = camera_read,
	.discoverFunc = camera_discover,
	.writeFunc = camera_write,
	.executeFunc = camera_execute,
};

/* Wakaama's Block2 responder serializes the resource on each request.
 * Lend it the stable JPEG for opaque and TLV responses instead of copying it.
 * Both the data node and the outgoing payload release through lwm2m_free.
 */
void halow_camera_free(void *value)
{
	if (value != jpeg && value != jpeg - JPEG_TLV_HEADER_SHORT && value != jpeg_storage) {
		free(value);
	}
}

int __real_lwm2m_data_serialize(lwm2m_uri_t *uri, int size, lwm2m_data_t *data,
				lwm2m_media_type_t *format, uint8_t **buffer);

int __wrap_lwm2m_data_serialize(lwm2m_uri_t *uri, int size, lwm2m_data_t *data,
				lwm2m_media_type_t *format, uint8_t **buffer)
{
	if (size == 1 && data->type == LWM2M_TYPE_OPAQUE &&
	    data->value.asBuffer.buffer == jpeg) {
		if (*format == LWM2M_CONTENT_OPAQUE) {
			*buffer = jpeg;
			return jpeg_length;
		}
		if (*format == LWM2M_CONTENT_TLV) {
			/* Preserve the 16-bit TLV header for JPEGs smaller than 64 KiB. */
			bool long_length = jpeg_length > UINT16_MAX;
			size_t header_length = long_length ? JPEG_TLV_HEADER_MAX : JPEG_TLV_HEADER_SHORT;
			uint8_t *header = jpeg - header_length;

			header[0] = long_length ? 0xd8 : 0xd0;
			header[1] = IMAGE;
			if (long_length) {
				sys_put_be24(jpeg_length, &header[2]);
			} else {
				sys_put_be16(jpeg_length, &header[2]);
			}
			*buffer = header;
			return jpeg_length + header_length;
		}
	}
	return __real_lwm2m_data_serialize(uri, size, data, format, buffer);
}
#endif

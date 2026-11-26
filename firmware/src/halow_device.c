/* SPDX-License-Identifier: Apache-2.0
 * Device /3/0 with real identity, battery-input voltage and system state.
 */
#include "halow_device.h"
#include "hardware_config.h"
#define EZ_DEV54_TYPE hardware_type_name()
#include "halow_sensors.h"
#include "sensors_hw.h"
#include "device_platform.h"
#include <string.h>
#include <zephyr/kernel.h>
#ifndef EZ_DEV54_APP_REV
#define EZ_DEV54_APP_REV "development"
#endif
static int64_t voltage_mv;
static bool voltage_valid, clock_valid;
static int64_t clock_epoch_ms, clock_uptime;
static char utc_offset[7] = "+00:00", device_timezone[64] = "Etc/UTC";

static bool supported(uint16_t resource)
{
	/* Current and ExtDevInfo have no physical source/linked Device object. */
	return resource <= 21 && resource != 8;
}
static bool executable(uint16_t resource) { return resource == 4 || resource == 5 || resource == 12; }
static uint8_t discover(lwm2m_context_t *ctx, uint16_t instance, int *count,
			lwm2m_data_t **data, lwm2m_object_t *object)
{
	(void)ctx; (void)object;
	if (instance) return COAP_404_NOT_FOUND;
	if (!*count) {
		*count = 21; /* IDs 0..21 except absent current resource 8 */
		*data = lwm2m_data_new(*count);
		if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
		int i = 0;
		for (uint16_t r = 0; r <= 21; ++r) if (supported(r)) (*data)[i++].id = r;
	}
	for (int i = 0; i < *count; ++i) if (!supported((*data)[i].id)) return COAP_404_NOT_FOUND;
	return COAP_205_CONTENT;
}
static int multi_integer(lwm2m_data_t *resource, int64_t value)
{
	lwm2m_data_t *child = lwm2m_data_new(1);
	if (!child) return -1;
	child->id = 0; lwm2m_data_encode_int(value, child);
	lwm2m_data_encode_instances(child, 1, resource);
	return 0;
}
static int battery_percent(int64_t mv)
{
	/* Operator policy: external >5 V means 100%; Li-ion 3.0..4.2 V is
	 * linear. Values outside the Li-ion range are clamped to 0..100%. */
	if (mv > 5000 || mv >= 4200) return 100;
	if (mv <= 3000) return 0;
	return (int)(((mv - 3000) * 100 + 600) / 1200);
}
static uint8_t read_device(lwm2m_context_t *ctx, uint16_t instance, int *count,
			  lwm2m_data_t **data, lwm2m_object_t *object)
{
	(void)ctx; (void)object;
	if (instance) return COAP_404_NOT_FOUND;
	if (!*count) {
		int n = 0;
		for (uint16_t r = 0; r <= 21; ++r)
			if (supported(r) && !executable(r) && (r != 13 || clock_valid)) ++n;
		*data = lwm2m_data_new(n);
		if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
		*count = n; n = 0;
		for (uint16_t r = 0; r <= 21; ++r)
			if (supported(r) && !executable(r) && (r != 13 || clock_valid)) (*data)[n++].id = r;
	}
	bool voltage_needed = false;
	for (int i = 0; i < *count; ++i) {
		uint16_t id = (*data)[i].id;
		if (!supported(id)) return COAP_404_NOT_FOUND;
		if (executable(id)) return COAP_405_METHOD_NOT_ALLOWED;
		if (id == 13 && !clock_valid) return COAP_503_SERVICE_UNAVAILABLE;
		voltage_needed |= id == 7 || id == 9;
	}
	if (voltage_needed && !halow_sensors_is_internal_read()) {
		double volts;
		voltage_valid = board_voltage_read(&volts) == 0;
		if (voltage_valid) voltage_mv = (int64_t)(volts * 1000.0 + 0.5);
	}
	if (voltage_needed && !voltage_valid) return COAP_503_SERVICE_UNAVAILABLE;
	for (int i = 0; i < *count; ++i) {
		lwm2m_data_t *r = &(*data)[i];
		switch (r->id) {
		case 0: lwm2m_data_encode_string("EdgeZ.ai", r); break;
		case 1: lwm2m_data_encode_string(board_device_model(), r); break;
		case 2: lwm2m_data_encode_string(board_device_serial(), r); break;
		case 3: lwm2m_data_encode_string(EZ_DEV54_APP_REV, r); break;
		case 6: if (multi_integer(r, 1)) return COAP_500_INTERNAL_SERVER_ERROR; break;
		case 7: if (multi_integer(r, voltage_mv)) return COAP_500_INTERNAL_SERVER_ERROR; break;
		case 9: lwm2m_data_encode_int(battery_percent(voltage_mv), r); break;
		case 10: lwm2m_data_encode_int(board_memory_free_kb(), r); break;
		case 11: if (multi_integer(r, board_device_error())) return COAP_500_INTERNAL_SERVER_ERROR; break;
		case 13: lwm2m_data_encode_int((clock_epoch_ms + k_uptime_get() - clock_uptime) / 1000, r); break;
		case 14: lwm2m_data_encode_string(utc_offset, r); break;
		case 15: lwm2m_data_encode_string(device_timezone, r); break;
		case 16: lwm2m_data_encode_string("U", r); break;
		case 17: lwm2m_data_encode_string(EZ_DEV54_TYPE, r); break;
		case 18: lwm2m_data_encode_string("nRF54L15/MM8108 carrier", r); break;
		case 19: lwm2m_data_encode_string(board_device_software(), r); break;
		case 20: lwm2m_data_encode_int(6, r); break; /* charger/status sensing absent */
		case 21: lwm2m_data_encode_int(board_memory_total_kb(), r); break;
		default: return COAP_404_NOT_FOUND;
		}
	}
	return COAP_205_CONTENT;
}
static int read_string(lwm2m_data_t *data, char *out, size_t capacity)
{
	if ((data->type != LWM2M_TYPE_OPAQUE && data->type != LWM2M_TYPE_STRING) ||
	    !data->value.asBuffer.buffer || !data->value.asBuffer.length || data->value.asBuffer.length >= capacity ||
	    memchr(data->value.asBuffer.buffer, 0, data->value.asBuffer.length)) return -1;
	memcpy(out, data->value.asBuffer.buffer, data->value.asBuffer.length);
	out[data->value.asBuffer.length] = 0;
	return 0;
}
static bool valid_offset(const char *s)
{
	return strlen(s) == 6 && (s[0] == '+' || s[0] == '-') && s[3] == ':' &&
		s[1] >= '0' && s[1] <= '2' && s[2] >= '0' && s[2] <= '9' &&
		(s[1]-'0')*10 + s[2]-'0' <= 23 && s[4] >= '0' && s[4] <= '5' && s[5] >= '0' && s[5] <= '9';
}
static uint8_t write_device(lwm2m_context_t *ctx, uint16_t instance, int count,
			   lwm2m_data_t *data, lwm2m_object_t *object, lwm2m_write_type_t type)
{
	(void)object; (void)type;
	if (instance) return COAP_404_NOT_FOUND;
	if (!count) return COAP_400_BAD_REQUEST;
	int64_t epoch = clock_epoch_ms / 1000;
	char offset[sizeof(utc_offset)], zone[sizeof(device_timezone)];
	memcpy(offset, utc_offset, sizeof(offset)); memcpy(zone, device_timezone, sizeof(zone));
	for (int i = 0; i < count; ++i) {
		switch (data[i].id) {
		case 13:
			if (!lwm2m_data_decode_int(&data[i], &epoch) || epoch < 0 || epoch > 253402300799LL) return COAP_400_BAD_REQUEST;
			break;
		case 14:
			if (read_string(&data[i], offset, sizeof(offset)) || !valid_offset(offset)) return COAP_400_BAD_REQUEST;
			break;
		case 15:
			if (read_string(&data[i], zone, sizeof(zone))) return COAP_400_BAD_REQUEST;
			break;
		default: return supported(data[i].id) ? COAP_405_METHOD_NOT_ALLOWED : COAP_404_NOT_FOUND;
		}
	}
	for (int i = 0; i < count; ++i) {
		if (data[i].id == 13) { clock_epoch_ms = epoch * 1000; clock_uptime = k_uptime_get(); clock_valid = true; }
		if (data[i].id == 14) memcpy(utc_offset, offset, sizeof(utc_offset));
		if (data[i].id == 15) memcpy(device_timezone, zone, sizeof(device_timezone));
		if (ctx) {
			lwm2m_uri_t uri; LWM2M_URI_RESET(&uri);
			uri.objectId = 3; uri.instanceId = 0; uri.resourceId = data[i].id;
			lwm2m_resource_value_changed(ctx, &uri);
		}
	}
	return COAP_204_CHANGED;
}
static uint8_t execute_device(lwm2m_context_t *ctx, uint16_t instance, uint16_t resource,
			    uint8_t *buffer, int length, lwm2m_object_t *object)
{
	(void)ctx; (void)buffer; (void)object;
	if (instance || !supported(resource)) return COAP_404_NOT_FOUND;
	if (!executable(resource)) return COAP_405_METHOD_NOT_ALLOWED;
	if (length) return COAP_400_BAD_REQUEST;
	if (resource == 12) board_device_reset_errors();
	else if (board_device_reboot(resource == 5)) return COAP_503_SERVICE_UNAVAILABLE;
	return COAP_204_CHANGED;
}
static lwm2m_list_t instance = {.id = 0};
lwm2m_object_t halow_device = {
	.objID = 3, .instanceList = &instance, .readFunc = read_device,
	.discoverFunc = discover, .writeFunc = write_device, .executeFunc = execute_device,
};

void halow_device_sync_clock(lwm2m_context_t *ctx, int64_t unix_ms)
{
	clock_epoch_ms = unix_ms;
	clock_uptime = k_uptime_get();
	clock_valid = true;
	if (ctx) {
		lwm2m_uri_t uri; LWM2M_URI_RESET(&uri);
		uri.objectId = 3; uri.instanceId = 0; uri.resourceId = 13;
		lwm2m_resource_value_changed(ctx, &uri);
	}
}


int halow_device_step(lwm2m_context_t *ctx)
{
	static int64_t deadline, last_second;
	bool watched = false;
	if (ctx && clock_valid) {
		for (lwm2m_observed_t *o = ctx->observedList; o; o = o->next) {
			if (o->uri.objectId != 3 ||
			    (LWM2M_URI_IS_SET_INSTANCE(&o->uri) && o->uri.instanceId != 0) ||
			    (LWM2M_URI_IS_SET_RESOURCE(&o->uri) && o->uri.resourceId != 13)) continue;
			for (lwm2m_watcher_t *w = o->watcherList; w; w = w->next) {
				lwm2m_server_t *s = w->server;
				if (w->active && s && s->sessionH &&
				    (s->status == STATE_REGISTERED || s->status == STATE_REG_UPDATE_PENDING ||
				     s->status == STATE_REG_UPDATE_NEEDED || s->status == STATE_REG_FULL_UPDATE_NEEDED)) {
					watched = true; break;
				}
			}
		}
	}
	if (!watched) { deadline = 0; return -1; }
	int64_t now = k_uptime_get();
	int64_t second = (clock_epoch_ms + now - clock_uptime) / 1000;
	if (!deadline) { deadline = now + 1000; last_second = second; }
	if (now >= deadline) {
		if (second != last_second) {
			lwm2m_uri_t uri; LWM2M_URI_RESET(&uri);
			uri.objectId = 3; uri.instanceId = 0; uri.resourceId = 13;
			lwm2m_resource_value_changed(ctx, &uri);
			last_second = second;
		}
		deadline = now + 1000;
	}
	return (int)(deadline - now);
}

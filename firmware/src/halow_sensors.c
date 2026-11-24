/* SPDX-License-Identifier: Apache-2.0
 * Standard IPSO objects; native Wakaama Observe owns pmin/pmax/thresholds.
 * No sampling work item runs when no active IMU watcher exists.
 */
#include "halow_sensors.h"
#include "sensors_hw.h"
#include <string.h>
#include <zephyr/kernel.h>

#define ACCEL_ID 3313
#define GYRO_ID 3334
static unsigned int streaming;
static int64_t next_sample;
static bool internal_read;
static bool axis_valid[2];
static double axis_cache[2][3];

void halow_sensors_internal_read(bool internal) { internal_read = internal; }
bool halow_sensors_is_internal_read(void) { return internal_read; }
static bool server_live(const lwm2m_server_t *server)
{
	if (!server || !server->sessionH) return false;
	return server->status == STATE_REGISTERED || server->status == STATE_REG_UPDATE_PENDING ||
		server->status == STATE_REG_UPDATE_NEEDED || server->status == STATE_REG_FULL_UPDATE_NEEDED;
}
static unsigned int observation_mask(lwm2m_context_t *ctx)
{
	unsigned int mask = 0;
	if (!ctx) return 0;
	for (lwm2m_observed_t *o = ctx->observedList; o; o = o->next) {
		unsigned int bit = o->uri.objectId == ACCEL_ID ? EZ_IMU_ACCEL :
			(o->uri.objectId == GYRO_ID ? EZ_IMU_GYRO : 0);
		if (!bit || (LWM2M_URI_IS_SET_INSTANCE(&o->uri) && o->uri.instanceId != 0) ||
		    (LWM2M_URI_IS_SET_RESOURCE(&o->uri) &&
		     (o->uri.resourceId < 5702 || o->uri.resourceId > 5704))) continue;
		for (lwm2m_watcher_t *w = o->watcherList; w; w = w->next) {
			/* Attribute-only watcher records have active=false. */
			if (w->active && server_live(w->server)) { mask |= bit; break; }
		}
	}
	return mask;
}
static void update_axes(lwm2m_context_t *ctx, unsigned int bit, const double values[3])
{
	unsigned int index = bit == EZ_IMU_ACCEL ? 0 : 1;
	for (unsigned int i = 0; i < 3; ++i) {
		if (ctx && (streaming & bit) && (!axis_valid[index] || axis_cache[index][i] != values[i])) {
			lwm2m_uri_t uri;
			LWM2M_URI_RESET(&uri);
			uri.objectId = index == 0 ? ACCEL_ID : GYRO_ID;
			uri.instanceId = 0; uri.resourceId = 5702 + i;
			lwm2m_resource_value_changed(ctx, &uri);
		}
		axis_cache[index][i] = values[i];
	}
	axis_valid[index] = true;
}
int halow_sensors_step(lwm2m_context_t *ctx)
{
	unsigned int wanted = observation_mask(ctx);
	if (wanted != streaming) {
		streaming = wanted;
		(void)board_imu_observe(wanted);
		next_sample = k_uptime_get();
	}
	if (!streaming) return -1;
	int64_t now = k_uptime_get();
	if (now >= next_sample) {
		for (unsigned int bit = EZ_IMU_ACCEL; bit <= EZ_IMU_GYRO; bit <<= 1) {
			if (streaming & bit) {
				double values[3];
				if (!board_imu_read(bit, values)) update_axes(ctx, bit, values);
				else axis_valid[bit == EZ_IMU_ACCEL ? 0 : 1] = false;
			}
		}
		next_sample = k_uptime_get() + CONFIG_EZ_CAM_IMU_OBSERVE_INTERVAL_MS;
	}
	int64_t remaining = next_sample - k_uptime_get();
	return remaining > 0 ? (int)remaining : 0;
}
static bool resource_known(uint16_t object, uint16_t resource)
{
	(void)object;
	return resource == 5701 || (resource >= 5702 && resource <= 5704);
}
static uint8_t discover(lwm2m_context_t *ctx, uint16_t instance, int *count,
			lwm2m_data_t **data, lwm2m_object_t *object)
{
	(void)ctx;
	if (instance != 0) return COAP_404_NOT_FOUND;
	if (!*count) {
		*count = 4;
		*data = lwm2m_data_new(*count);
		if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
		(*data)[0].id = 5701;
		for (int i = 1; i < *count; ++i) (*data)[i].id = 5701 + i;
	}
	for (int i = 0; i < *count; ++i) {
		if (!resource_known(object->objID, (*data)[i].id)) return COAP_404_NOT_FOUND;
	}
	return COAP_205_CONTENT;
}
static uint8_t read_sensor(lwm2m_context_t *ctx, uint16_t instance, int *count,
			  lwm2m_data_t **data, lwm2m_object_t *object)
{
	uint8_t result = discover(ctx, instance, count, data, object);
	if (result != COAP_205_CONTENT) return result;
	bool measurement = false;
	for (int i = 0; i < *count; ++i) measurement |= (*data)[i].id != 5701;
	unsigned int bit = object->objID == ACCEL_ID ? EZ_IMU_ACCEL : EZ_IMU_GYRO;
	unsigned int index = bit == EZ_IMU_ACCEL ? 0 : 1;
	if (measurement && !internal_read) {
		/* Direct Read/initial Observe response always takes a current sample.
		 * Subsequent native observe_step reads use the sampler's cache: merely
		 * writing pmin/pmax must not create an implicit hardware sampling loop. */
		double values[3];
		if (!board_imu_read(bit, values)) update_axes(ctx, bit, values);
		else axis_valid[index] = false;
	}
	if (measurement && !axis_valid[index]) return COAP_503_SERVICE_UNAVAILABLE;
	for (int i = 0; i < *count; ++i) {
		lwm2m_data_t *r = &(*data)[i];
		if (r->id == 5701) lwm2m_data_encode_string(index == 0 ? "m/s2" : "rad/s", r);
		else lwm2m_data_encode_float(axis_cache[index][r->id - 5702], r);
	}
	return COAP_205_CONTENT;
}
static lwm2m_list_t accel_instance = {.id = 0}, gyro_instance = {.id = 0};
lwm2m_object_t halow_accelerometer = {
	.objID = ACCEL_ID, .instanceList = &accel_instance, .readFunc = read_sensor, .discoverFunc = discover,
};
lwm2m_object_t halow_gyrometer = {
	.objID = GYRO_ID, .instanceList = &gyro_instance, .readFunc = read_sensor, .discoverFunc = discover,
};

/* SPDX-License-Identifier: Apache-2.0 */
#if defined(CONFIG_EZ_DEV54_UART_GPS)
#include "halow_gps.h"
#include "gps_hw.h"
#include "gps_nmea.h"
#include "gps_diagnostics.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>

enum gps_state { IDLE, ACQUIRING, FIXED, TIMED_OUT, ERROR, CANCELLED };
static enum gps_state state;
static struct gps_fix fix;
static bool valid, powered, ready;
/* Once per MCU boot; never delay publishing the first location. */
static bool first_fix_seen;
static int64_t keep_powered_until;
static int last_result;
static uint32_t fixes;
static int64_t timeout_s = CONFIG_EZ_DEV54_GPS_FIX_TIMEOUT_SECONDS, deadline, fix_at;
static char line[128];
static size_t used;
volatile struct gps_acquisition_diagnostics gps_acquisition_diagnostics;
static void changed(lwm2m_context_t *ctx, uint16_t object, uint16_t resource)
{
    if (!ctx) return;
    lwm2m_uri_t uri; LWM2M_URI_RESET(&uri);
    uri.objectId = object; uri.instanceId = 0; uri.resourceId = resource;
    lwm2m_resource_value_changed(ctx, &uri);
}
static void finished(lwm2m_context_t *ctx, enum gps_state next, int result)
{
    int rc = gps_hw_stop(); powered = false; used = 0; keep_powered_until = 0;
    state = rc ? ERROR : next; last_result = rc ? rc : result;
    gps_acquisition_diagnostics.finished_ms = (uint32_t)k_uptime_get();
    gps_acquisition_diagnostics.state = state;
    gps_acquisition_diagnostics.result = last_result;
    for (uint16_t r = 2; r <= 6; ++r) changed(ctx, 31028, r);
}
bool halow_gps_result(struct gps_fix *out, int *result)
{
    if (state == ACQUIRING) return false;
    *result = state == FIXED && valid ? 0 : last_result ? last_result : -ENODATA;
    if (!*result) *out = fix;
    return true;
}
int halow_gps_init(void)
{
    int rc = gps_hw_init(); ready = rc == 0;
    if (rc) { state = ERROR; last_result = rc; }
    return rc;
}
int halow_gps_step(lwm2m_context_t *ctx, bool allow)
{
    if (state != ACQUIRING && !(state == FIXED && powered)) return -1;
    if (!allow) {
        finished(ctx, state == FIXED ? FIXED : CANCELLED, state == FIXED ? 0 : -ECANCELED);
        return -1;
    }
    if (state == FIXED && k_uptime_get() >= keep_powered_until) {
        finished(ctx, FIXED, 0); return -1;
    }
    if (!powered) {
        gps_acquisition_diagnostics.first_step_ms = (uint32_t)k_uptime_get();
        int rc = gps_hw_start();
        if (rc) { finished(ctx, ERROR, rc); return -1; }
        powered = true;
    }
    if (state == ACQUIRING && k_uptime_get() >= deadline) { finished(ctx, TIMED_OUT, -ETIMEDOUT); return -1; }
    uint8_t bytes[128]; int count;
    for (unsigned int budget = 0; budget < 512; budget += (unsigned int)count) {
        count = gps_hw_read(bytes, sizeof(bytes));
        if (count < 0) { finished(ctx, ERROR, count); return -1; }
        if (!count) break;
        for (int i = 0; i < count; ++i) {
            if (bytes[i] == '$') { used = 0; line[used++] = '$'; }
            else if (bytes[i] == '\n' && used) {
                line[used] = 0; used = 0;
                ++gps_acquisition_diagnostics.lines;
                if (gps_acquisition_diagnostics.first_line_ms == UINT32_MAX)
                    gps_acquisition_diagnostics.first_line_ms = (uint32_t)k_uptime_get();
                memcpy((void *)gps_acquisition_diagnostics.last_line, line, sizeof(line));
                bool rmc = !strncmp(line, "$GNRMC,", 7) || !strncmp(line, "$GPRMC,", 7);
                if (rmc) {
                    ++gps_acquisition_diagnostics.rmc_lines;
                    memcpy((void *)gps_acquisition_diagnostics.last_rmc, line, sizeof(line));
                    const char *status = strchr(line + 7, ',');
                    if (status && status[1] == 'A' && status[2] == ',') ++gps_acquisition_diagnostics.active_rmc_lines;
                }
                struct gps_fix next;
                bool parsed = gps_nmea_fix(line, &next);
                if (rmc && !parsed) ++gps_acquisition_diagnostics.rejected_rmc_lines;
                if (parsed && state == ACQUIRING) {
                    gps_acquisition_diagnostics.fixed_ms = (uint32_t)k_uptime_get();
                    fix = next; valid = true; fix_at = k_uptime_get(); ++fixes;
                    if (!first_fix_seen) {
                        first_fix_seen = true;
                        keep_powered_until = fix_at + 60000;
                    }
                    if (k_uptime_get() < keep_powered_until) {
                        state = FIXED; last_result = 0;
                        gps_acquisition_diagnostics.state = state;
                        gps_acquisition_diagnostics.result = 0;
                        for (uint16_t r = 2; r <= 6; ++r) changed(ctx, 31028, r);
                    } else finished(ctx, FIXED, 0);
                    changed(ctx, 6, 0); changed(ctx, 6, 1); changed(ctx, 6, 5);
                    return powered ? (int)(keep_powered_until - k_uptime_get()) : -1;
                }
            } else if (used) {
                if (used < sizeof(line) - 1) line[used++] = bytes[i];
                else used = 0; /* Discard an overlong sentence until the next '$'. */
            }
        }
    }
    /* Drain UART during the first-fix hold so interrupts cannot fill the ring. */
    int64_t wait = (state == FIXED ? keep_powered_until : deadline) - k_uptime_get();
    return wait <= 0 ? 0 : (int)wait;
}
static uint8_t discover_location(lwm2m_context_t *ctx, uint16_t instance, int *count,
                                lwm2m_data_t **data, lwm2m_object_t *object)
{
    (void)ctx; (void)object;
    if (instance) return COAP_404_NOT_FOUND;
    if (!*count) {
        *data = lwm2m_data_new(3); if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
        *count = 3; (*data)[0].id = 0; (*data)[1].id = 1; (*data)[2].id = 5;
    }
    for (int i = 0; i < *count; ++i)
        if ((*data)[i].id != 0 && (*data)[i].id != 1 && (*data)[i].id != 5) return COAP_404_NOT_FOUND;
    return COAP_205_CONTENT;
}
static uint8_t read_location(lwm2m_context_t *ctx, uint16_t instance, int *count,
                            lwm2m_data_t **data, lwm2m_object_t *object)
{
    uint8_t rc = discover_location(ctx, instance, count, data, object);
    if (rc != COAP_205_CONTENT) return rc;
    if (!valid) return COAP_503_SERVICE_UNAVAILABLE;
    for (int i = 0; i < *count; ++i) {
        if ((*data)[i].id == 5) lwm2m_data_encode_int(fix.timestamp, &(*data)[i]);
        else lwm2m_data_encode_float((*data)[i].id == 0 ? fix.latitude : fix.longitude, &(*data)[i]);
    }
    return rc;
}
static uint8_t discover_control(lwm2m_context_t *ctx, uint16_t instance, int *count,
                               lwm2m_data_t **data, lwm2m_object_t *object)
{
    (void)ctx; (void)object;
    if (instance) return COAP_404_NOT_FOUND;
    if (!*count) {
        *data = lwm2m_data_new(7); if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
        *count = 7; for (int i = 0; i < 7; ++i) (*data)[i].id = i;
    }
    for (int i = 0; i < *count; ++i) if ((*data)[i].id > 6) return COAP_404_NOT_FOUND;
    return COAP_205_CONTENT;
}
static uint8_t read_control(lwm2m_context_t *ctx, uint16_t instance, int *count,
                           lwm2m_data_t **data, lwm2m_object_t *object)
{
    if (!*count) {
        if (instance) return COAP_404_NOT_FOUND;
        *data = lwm2m_data_new(6); if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
        *count = 6; for (int i = 0; i < 6; ++i) (*data)[i].id = i + 1;
    }
    uint8_t rc = discover_control(ctx, instance, count, data, object);
    if (rc != COAP_205_CONTENT) return rc;
    int64_t values[] = {0, timeout_s, state, last_result, valid ? (k_uptime_get() - fix_at) / 1000 : -1, fixes, valid};
    for (int i = 0; i < *count; ++i) {
        uint16_t rid = (*data)[i].id;
        if (!rid) return COAP_405_METHOD_NOT_ALLOWED;
        if (rid == 6) lwm2m_data_encode_bool(valid, &(*data)[i]);
        else lwm2m_data_encode_int(values[rid], &(*data)[i]);
    }
    return rc;
}
static uint8_t write_control(lwm2m_context_t *ctx, uint16_t instance, int count,
                            lwm2m_data_t *data, lwm2m_object_t *object, lwm2m_write_type_t type)
{
    (void)object; (void)type;
    if (instance) return COAP_404_NOT_FOUND;
    if (count != 1) return COAP_400_BAD_REQUEST;
    if (data[0].id != 1) return data[0].id <= 6 ? COAP_405_METHOD_NOT_ALLOWED : COAP_404_NOT_FOUND;
    if (state == ACQUIRING) return COAP_503_SERVICE_UNAVAILABLE;
    int64_t value;
    if (!lwm2m_data_decode_int(data, &value) || value < 10 || value > 600) return COAP_400_BAD_REQUEST;
    timeout_s = value; changed(ctx, 31028, 1); return COAP_204_CHANGED;
}
static uint8_t execute_control(lwm2m_context_t *ctx, uint16_t instance, uint16_t resource,
                              uint8_t *buffer, int length, lwm2m_object_t *object)
{
    (void)buffer; (void)object;
    if (instance || resource > 6) return COAP_404_NOT_FOUND;
    if (resource) return COAP_405_METHOD_NOT_ALLOWED;
    int64_t requested_timeout = timeout_s;
    if (length) {
        /* LwM2M Execute argument 0: timeout seconds, also opts into a deferred
         * location response. Empty Execute retains the legacy immediate ACK. */
        if (!buffer || length < 4 || length > 7 || buffer[0] != '0' || buffer[1] != '=') return COAP_400_BAD_REQUEST;
        int first = 2, end = length;
        if (buffer[first] == '\'') {
            if (buffer[length - 1] != '\'') return COAP_400_BAD_REQUEST;
            ++first; --end;
        }
        if (end - first < 2 || end - first > 3) return COAP_400_BAD_REQUEST;
        requested_timeout = 0;
        for (int i = first; i < end; ++i) {
            if (buffer[i] < '0' || buffer[i] > '9') return COAP_400_BAD_REQUEST;
            requested_timeout = requested_timeout * 10 + buffer[i] - '0';
        }
        if (requested_timeout < 10 || requested_timeout > 600) return COAP_400_BAD_REQUEST;
    }
    if (!ready || state == ACQUIRING) return COAP_503_SERVICE_UNAVAILABLE;
    uint32_t sequence = gps_acquisition_diagnostics.sequence + 1;
    memset((void *)&gps_acquisition_diagnostics, 0, sizeof(gps_acquisition_diagnostics));
    gps_acquisition_diagnostics.sequence = sequence;
    gps_acquisition_diagnostics.requested_ms = (uint32_t)k_uptime_get();
    gps_acquisition_diagnostics.first_step_ms = gps_acquisition_diagnostics.first_line_ms = UINT32_MAX;
    gps_acquisition_diagnostics.fixed_ms = gps_acquisition_diagnostics.finished_ms = UINT32_MAX;
    gps_acquisition_diagnostics.state = ACQUIRING;
    state = ACQUIRING; last_result = 0; used = 0; deadline = k_uptime_get() + requested_timeout * 1000;
    changed(ctx, 31028, 2); return COAP_204_CHANGED;
}
static lwm2m_list_t instance = {.id = 0};
lwm2m_object_t halow_gps_location = {
    .objID = 6, .instanceList = &instance, .readFunc = read_location, .discoverFunc = discover_location,
};
lwm2m_object_t halow_gps_control = {
    .objID = 31028, .instanceList = &instance, .readFunc = read_control, .discoverFunc = discover_control,
    .writeFunc = write_control, .executeFunc = execute_control,
};
#endif

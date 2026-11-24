/* SPDX-License-Identifier: Apache-2.0 */
#include "halow_hardware.h"
#include "hardware_config.h"
#include <errno.h>
static uint8_t discover(lwm2m_context_t *ctx, uint16_t instance, int *count,
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
static uint8_t read_config(lwm2m_context_t *ctx, uint16_t instance, int *count,
                           lwm2m_data_t **data, lwm2m_object_t *object)
{
    if (!*count) {
        if (instance) return COAP_404_NOT_FOUND;
        *data = lwm2m_data_new(6); if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
        *count = 6;
        const uint16_t ids[] = {0,1,2,3,4,6};
        for (int i = 0; i < 6; ++i) (*data)[i].id = ids[i];
    }
    uint8_t rc = discover(ctx, instance, count, data, object);
    if (rc != COAP_205_CONTENT) return rc;
    int64_t values[] = {hardware_config_desired(), hardware_mode_get(), hardware_config_factory(),
                       hardware_config_source(), hardware_config_pending(), 0, hardware_config_error()};
    for (int i = 0; i < *count; ++i) {
        uint16_t rid = (*data)[i].id;
        if (rid == 5) return COAP_405_METHOD_NOT_ALLOWED;
        if (rid == 4) lwm2m_data_encode_bool(values[rid], &(*data)[i]);
        else lwm2m_data_encode_int(values[rid], &(*data)[i]);
    }
    return rc;
}
static uint8_t write_config(lwm2m_context_t *ctx, uint16_t instance, int count,
                            lwm2m_data_t *data, lwm2m_object_t *object, lwm2m_write_type_t type)
{
    (void)ctx; (void)object; (void)type;
    if (instance) return COAP_404_NOT_FOUND;
    if (count != 1) return COAP_400_BAD_REQUEST;
    if (data[0].id) return data[0].id <= 6 ? COAP_405_METHOD_NOT_ALLOWED : COAP_404_NOT_FOUND;
    int64_t mode;
    if (!lwm2m_data_decode_int(data, &mode) || mode < 0 || mode > 3) return COAP_400_BAD_REQUEST;
    return hardware_config_set((int)mode) ? COAP_500_INTERNAL_SERVER_ERROR : COAP_204_CHANGED;
}
static uint8_t execute_config(lwm2m_context_t *ctx, uint16_t instance, uint16_t resource,
                              uint8_t *buffer, int length, lwm2m_object_t *object)
{
    (void)ctx; (void)buffer; (void)object;
    if (instance || resource > 6) return COAP_404_NOT_FOUND;
    if (resource != 5) return COAP_405_METHOD_NOT_ALLOWED;
    if (length) return COAP_400_BAD_REQUEST;
    return hardware_config_clear() ? COAP_500_INTERNAL_SERVER_ERROR : COAP_204_CHANGED;
}
static lwm2m_list_t instance = {.id = 0};
lwm2m_object_t halow_hardware = {
    .objID = 31029, .instanceList = &instance, .readFunc = read_config, .discoverFunc = discover,
    .writeFunc = write_config, .executeFunc = execute_config,
};

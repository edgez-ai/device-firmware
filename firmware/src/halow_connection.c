/* SPDX-License-Identifier: Apache-2.0
 * Connection watchdog: use native registration updates, rebuild only after a
 * missed response, then reconnect Wi-Fi. No probes/timers during WNM sleep.
 */
#include "halow_connection.h"
#include <limits.h>
#include <string.h>
#include <zephyr/kernel.h>
#define PROBE_MS ((int64_t)CONFIG_EZ_CAM_CONNECTION_PROBE_SECONDS * 1000)
#define RESPONSE_MS ((int64_t)CONFIG_EZ_CAM_CONNECTION_RESPONSE_SECONDS * 1000)
#define RECONNECT_MS 120000
#define MAX_BACKOFF_MS 300000
/* Object state: offline, registering, online, probing, re-registering,
 * reconnecting, WNM asleep. Recovery reasons: none, expired, timeout, rejected,
 * link unavailable. */
enum state { OFFLINE, REGISTERING, ONLINE, PROBING, REGISTER_AGAIN, RECONNECTING, ASLEEP };
enum reason { NO_REASON, EXPIRED, TIMED_OUT, REJECTED, LINK_DOWN };
static enum state state;
static enum reason reason;
static bool initialized, paused, wake_pending, seen_valid;
static int64_t deadline, last_ack_ms = -1;
static time_t seen_registration, lifetime;
static uint32_t registration_resets, wifi_reconnects;
static unsigned int recovery_stage;
static int64_t reconnect_backoff = RECONNECT_MS;
static lwm2m_server_t *server(lwm2m_context_t *ctx)
{
    if (!ctx) return NULL;
    for (lwm2m_server_t *s = ctx->serverList; s; s = s->next)
        if (s->shortID == 1) return s;
    return NULL;
}
void halow_connection_context_closed(void) { seen_valid = false; }
void halow_connection_pause(void)
{
    paused = true; state = ASLEEP; deadline = 0;
}
void halow_connection_wake(void)
{
    paused = false; wake_pending = true; initialized = true;
}
static enum halow_connection_action reconnect(int64_t now)
{
    ++wifi_reconnects; recovery_stage = 2; state = RECONNECTING;
    deadline = now + reconnect_backoff;
    reconnect_backoff = reconnect_backoff >= MAX_BACKOFF_MS / 2 ? MAX_BACKOFF_MS : reconnect_backoff * 2;
    return HALOW_CONN_RECONNECT;
}
static enum halow_connection_action register_again(int64_t now)
{
    ++registration_resets; recovery_stage = 1; state = REGISTER_AGAIN;
    deadline = now + RESPONSE_MS; seen_valid = false;
    return HALOW_CONN_REGISTER;
}
enum halow_connection_action halow_connection_step(lwm2m_context_t *ctx, bool online)
{
    if (paused) return HALOW_CONN_NONE;
    int64_t now = k_uptime_get();
    lwm2m_server_t *s = server(ctx);
    if (s) lifetime = s->lifetime;
    if (!initialized) {
        initialized = true; deadline = now + (online ? RESPONSE_MS : RECONNECT_MS);
        state = online ? REGISTERING : OFFLINE;
    }
    if (online && state == OFFLINE && recovery_stage == 0) {
        state = REGISTERING; deadline = now + RESPONSE_MS;
    }
    if (wake_pending) {
        wake_pending = false;
        if (!online) { reason = LINK_DOWN; return reconnect(now); }
        if (!s || s->status == STATE_REG_FAILED ||
            (s->lifetime > 0 && now / 1000 >= s->registration + s->lifetime)) {
            reason = s && s->status != STATE_REG_FAILED ? EXPIRED : REJECTED;
            return register_again(now);
        }
        /* Demand a new ACK; the pre-sleep REGISTERED state is not evidence
         * that the AP or server still knows this client. */
        seen_registration = s->registration; seen_valid = true;
        state = PROBING; recovery_stage = 0; deadline = now + RESPONSE_MS;
        return HALOW_CONN_UPDATE;
    }
    if (!online) {
        state = recovery_stage == 2 ? RECONNECTING : OFFLINE;
        if (now >= deadline) { reason = LINK_DOWN; return reconnect(now); }
        return HALOW_CONN_NONE;
    }
    if (s && s->status == STATE_REGISTERED &&
        (!seen_valid || s->registration != seen_registration)) {
        seen_registration = s->registration; seen_valid = true;
        last_ack_ms = now; state = ONLINE; recovery_stage = 0;
        reconnect_backoff = RECONNECT_MS; deadline = now + PROBE_MS;
        return HALOW_CONN_NONE;
    }
    bool rejected = s && (s->status == STATE_REG_FAILED ||
                          (ctx && ctx->state == STATE_BOOTSTRAP_REQUIRED));
    if (recovery_stage == 0 && rejected) {
        reason = REJECTED; return register_again(now);
    }
    if (now < deadline) return HALOW_CONN_NONE;
    if (state == ONLINE && s && s->status == STATE_REGISTERED) {
        state = PROBING; deadline = now + RESPONSE_MS;
        return HALOW_CONN_UPDATE;
    }
    reason = rejected ? REJECTED : TIMED_OUT;
    if (recovery_stage == 0) return register_again(now);
    return reconnect(now);
}
int halow_connection_wait_ms(void)
{
    if (paused) return -1;
    int64_t ms = deadline - k_uptime_get();
    return ms <= 0 ? 0 : ms > INT_MAX ? INT_MAX : (int)ms;
}
static uint8_t discover(lwm2m_context_t *ctx, uint16_t instance, int *count,
                        lwm2m_data_t **data, lwm2m_object_t *object)
{
    (void)ctx; (void)object;
    if (instance) return COAP_404_NOT_FOUND;
    if (!*count) {
        *data = lwm2m_data_new(8);
        if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
        *count = 8;
        for (int i = 0; i < 8; ++i) (*data)[i].id = i;
    }
    for (int i = 0; i < *count; ++i) if ((*data)[i].id > 7) return COAP_404_NOT_FOUND;
    return COAP_205_CONTENT;
}
static uint8_t read_watchdog(lwm2m_context_t *ctx, uint16_t instance, int *count,
                            lwm2m_data_t **data, lwm2m_object_t *object)
{
    uint8_t rc = discover(ctx, instance, count, data, object);
    if (rc != COAP_205_CONTENT) return rc;
    const int64_t values[] = { state, CONFIG_EZ_CAM_CONNECTION_PROBE_SECONDS,
        CONFIG_EZ_CAM_CONNECTION_RESPONSE_SECONDS, last_ack_ms < 0 ? -1 : (k_uptime_get()-last_ack_ms)/1000,
        registration_resets, wifi_reconnects, reason, lifetime };
    for (int i = 0; i < *count; ++i) lwm2m_data_encode_int(values[(*data)[i].id], &(*data)[i]);
    return COAP_205_CONTENT;
}
static lwm2m_list_t instance = {.id = 0};
lwm2m_object_t halow_connection = {
    .objID = 31027, .instanceList = &instance, .readFunc = read_watchdog, .discoverFunc = discover,
};

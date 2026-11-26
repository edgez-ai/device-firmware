/* SPDX-License-Identifier: Apache-2.0 */
#if defined(CONFIG_EZ_DEV54_UART_GPS)
#include "gps_execute.h"
#include "halow_gps.h"
#include "gps_nmea.h"
#include "internals.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>

/* One on-demand acquisition, with request identity retained for retransmitted
 * CONs. The confirmable final response owns its payload in Wakaama's transaction
 * list; no borrowed packet, server or UART pointers survive the callback. */
static struct {
    lwm2m_context_t *ctx;
    void *peer;
    uint16_t mid;
    uint8_t token[COAP_TOKEN_LEN], token_len;
    bool waiting;
    int64_t expires;
} request;

uint8_t __real_dm_handleRequest(lwm2m_context_t *, lwm2m_uri_t *, lwm2m_server_t *, coap_packet_t *, coap_packet_t *);

static void acknowledge(lwm2m_context_t *ctx, void *peer, coap_packet_t *message)
{
    if (message->type != COAP_TYPE_CON) return;
    coap_packet_t ack;
    coap_init_message(&ack, COAP_TYPE_ACK, 0, message->mid);
    (void)message_send(ctx, &ack, peer);
}

uint8_t __wrap_dm_handleRequest(lwm2m_context_t *ctx, lwm2m_uri_t *uri, lwm2m_server_t *server,
                               coap_packet_t *message, coap_packet_t *response)
{
    bool gps = message->code == COAP_POST && uri->objectId == 31028 &&
        LWM2M_URI_IS_SET_INSTANCE(uri) && uri->instanceId == 0 &&
        LWM2M_URI_IS_SET_RESOURCE(uri) && uri->resourceId == 0 &&
        !LWM2M_URI_IS_SET_RESOURCE_INSTANCE(uri) && message->payload_len > 0 &&
        (!IS_OPTION(message, COAP_OPTION_CONTENT_TYPE) || message->content_type == LWM2M_CONTENT_TEXT);
    if (!gps) return __real_dm_handleRequest(ctx, uri, server, message, response);
    /* The packet dispatcher has already checked server identity and ACL. Leave
     * registration/content/argument validation to the real DM/object callback. */
    if (server->status != STATE_REGISTERED && server->status != STATE_REG_UPDATE_NEEDED &&
        server->status != STATE_REG_FULL_UPDATE_NEEDED && server->status != STATE_REG_UPDATE_PENDING)
        return __real_dm_handleRequest(ctx, uri, server, message, response);
    if (!message->token_len) return COAP_400_BAD_REQUEST;
    if (request.ctx == ctx && request.expires > k_uptime_get() && request.mid == message->mid &&
        request.token_len == message->token_len && !memcmp(request.token, message->token, message->token_len) &&
        lwm2m_session_is_equal(request.peer, server->sessionH, ctx->userData)) {
        acknowledge(ctx, server->sessionH, message);
        return COAP_IGNORE;
    }
    if (request.waiting) return COAP_503_SERVICE_UNAVAILABLE;
    uint8_t status = __real_dm_handleRequest(ctx, uri, server, message, response);
    if (status != COAP_204_CHANGED) return status;
    request.ctx = ctx; request.peer = server->sessionH; request.mid = message->mid;
    request.token_len = message->token_len;
    memcpy(request.token, message->token, message->token_len);
    request.waiting = true;
    request.expires = k_uptime_get() + 900000;
    acknowledge(ctx, server->sessionH, message);
    return COAP_IGNORE;
}

void gps_execute_step(lwm2m_context_t *ctx)
{
    if (!ctx || request.ctx != ctx) { memset(&request, 0, sizeof(request)); return; }
    if (!request.waiting) return;
    struct gps_fix fix; int result;
    if (!halow_gps_result(&fix, &result)) return;
    uint8_t status = result == 0 ? COAP_204_CHANGED : COAP_503_SERVICE_UNAVAILABLE;
    char payload[192], latitude[24], longitude[24];
    if (result == 0) {
        uint32_t lat = (uint32_t)(fabs(fix.latitude) * 10000000 + 0.5);
        uint32_t lon = (uint32_t)(fabs(fix.longitude) * 10000000 + 0.5);
        snprintf(latitude, sizeof(latitude), "%s%lu.%07lu", fix.latitude < 0 ? "-" : "", (unsigned long)(lat / 10000000), (unsigned long)(lat % 10000000));
        snprintf(longitude, sizeof(longitude), "%s%lu.%07lu", fix.longitude < 0 ? "-" : "", (unsigned long)(lon / 10000000), (unsigned long)(lon % 10000000));
    }
    int length = result == 0 ? snprintf(payload, sizeof(payload),
        "{\"latitude\":%s,\"longitude\":%s,\"timestamp\":%lld}",
        latitude, longitude, (long long)fix.timestamp) :
        snprintf(payload, sizeof(payload), "{\"error\":\"GPS acquisition failed\",\"result\":%d}", result);
    if (length < 0 || (size_t)length >= sizeof(payload)) return;
    lwm2m_transaction_t *tx = transaction_new(request.peer, (coap_method_t)status, NULL, NULL,
        ctx->nextMID++, request.token_len, request.token);
    if (!tx) return; /* Retry allocation on the next event-loop iteration. */
    coap_set_header_content_type(tx->message, LWM2M_CONTENT_TEXT); /* JSON text Execute result */
    if (!transaction_set_payload(tx, (uint8_t *)payload, (size_t)length)) { transaction_free(tx); return; }
    ctx->transactionList = (lwm2m_transaction_t *)LWM2M_LIST_ADD(ctx->transactionList, tx);
    request.waiting = false;
    request.expires = k_uptime_get() + 247000;
    (void)transaction_send(ctx, tx);
}
#endif

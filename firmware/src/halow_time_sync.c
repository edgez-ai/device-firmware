/* SPDX-License-Identifier: Apache-2.0 */
#include "halow_time_sync.h"
#include "halow_device.h"
#include "er-coap-13/er-coap-13.h"
#include <ctype.h>
#include <zephyr/kernel.h>
#include <string.h>

#define RTT_WINDOW 8
static struct {
	void *peer;
	uint8_t token[8], token_len;
	uint16_t mid;
	int64_t sent_at;
	bool valid, retransmitted;
} tx;
static uint32_t rtt_samples[RTT_WINDOW], rtt_sum;
static unsigned int rtt_count, rtt_cursor;

/* Record only successful registration sends, not unrelated Device traffic. */
void halow_time_sync_sent(void *peer, const uint8_t *bytes, size_t length)
{
	if (!bytes || length < 4 || length > 1024 || bytes[1] != COAP_POST) return;
	uint8_t copy[1024]; memcpy(copy, bytes, length);
	coap_packet_t request = {0};
	if (coap_parse_message(&request, copy, (uint16_t)length) == COAP_NO_ERROR &&
	    request.token_len && request.uri_path && request.uri_path->len == 2 &&
	    memcmp(request.uri_path->data, "rd", 2) == 0) {
		if (tx.valid && tx.peer == peer && tx.mid == request.mid &&
		    tx.token_len == request.token_len && !memcmp(tx.token, request.token, tx.token_len)) {
			/* A reply cannot identify which retransmission it acknowledges. */
			tx.retransmitted = true;
		} else {
			tx.peer = peer; tx.mid = request.mid; tx.token_len = request.token_len;
			memcpy(tx.token, request.token, request.token_len);
			tx.sent_at = k_uptime_get(); tx.valid = true; tx.retransmitted = false;
		}
	}
	coap_free_header(&request);
}
static int64_t estimated_downlink_ms(void *peer, const coap_packet_t *reply)
{
	if (tx.valid && tx.peer == peer && tx.token_len == reply->token_len &&
	    !memcmp(tx.token, reply->token, tx.token_len)) {
		int64_t elapsed = k_uptime_get() - tx.sent_at;
		if (!tx.retransmitted && elapsed >= 0 && elapsed <= 60000) {
			if (rtt_count == RTT_WINDOW) rtt_sum -= rtt_samples[rtt_cursor];
			else ++rtt_count;
			rtt_samples[rtt_cursor] = (uint32_t)elapsed;
			rtt_sum += (uint32_t)elapsed;
			rtt_cursor = (rtt_cursor + 1) % RTT_WINDOW;
		}
		tx.valid = false;
	}
	return rtt_count ? rtt_sum / (2U * rtt_count) : 0;
}

/* Optional integer metadata in the existing EdgeZ registration ACK. Do not
 * interpret uptime/week counters as UTC or infer a year from a stale build. */
static int64_t ack_unix_ms(const uint8_t *payload, size_t length, bool first_block)
{
	if (!payload || !length || length > 512 || memchr(payload, 0, length)) return -1;
	char json[513];
	memcpy(json, payload, length); json[length] = 0;
	char *begin = json, *end = json + length;
	while (begin < end && isspace((unsigned char)*begin)) ++begin;
	while (end > begin && isspace((unsigned char)end[-1])) --end;
	if (end - begin < 2 || *begin != '{' || (!first_block && end[-1] != '}')) return -1;
	const char key[] = "\"server_unix_ms\"";
	char *field = strstr(begin, key);
	if (!field || strstr(field + sizeof(key) - 1, key)) return -1;
	char *previous = field;
	while (previous > begin && isspace((unsigned char)previous[-1])) --previous;
	if (previous == begin || (previous[-1] != '{' && previous[-1] != ',')) return -1;
	char *p = field + sizeof(key) - 1;
	while (p < end && isspace((unsigned char)*p)) ++p;
	if (p == end || *p++ != ':') return -1;
	while (p < end && isspace((unsigned char)*p)) ++p;
	if (p == end || !isdigit((unsigned char)*p)) return -1;
	int64_t value = 0;
	while (p < end && isdigit((unsigned char)*p)) {
		if (value > (4102444800000LL - (*p-'0')) / 10) return -1;
		value = value * 10 + (*p++ - '0');
	}
	while (p < end && isspace((unsigned char)*p)) ++p;
	if (p == end || (*p != ',' && *p != '}')) return -1;
	/* Reject unset gateway clocks (1970) and implausible future dates. */
	return value >= 1577836800000LL ? value : -1;
}

static __attribute__((noinline)) int64_t registration_time(lwm2m_context_t *ctx, uint8_t *bytes, size_t length, void *peer)
{
	if (!ctx || !bytes || length < 4 || length > 1024 ||
	    (bytes[1] != COAP_201_CREATED && bytes[1] != COAP_204_CHANGED)) return -1;

	bool pending = false;
	for (lwm2m_server_t *s = ctx->serverList; s; s = s->next) {
		if (s->sessionH == peer &&
		    ((s->status == STATE_REG_PENDING && bytes[1] == COAP_201_CREATED) ||
		     (s->status == STATE_REG_UPDATE_PENDING && bytes[1] == COAP_204_CHANGED))) pending = true;
	}
	if (!pending) return -1;

	/* The CoAP parser can merge options in place; leave the original packet
	 * untouched for the real Wakaama dispatch below. */
	uint8_t copy[1024]; memcpy(copy, bytes, length);
	coap_packet_t reply = {0};
	int64_t unix_ms = -1;
	if (coap_parse_message(&reply, copy, (uint16_t)length) == COAP_NO_ERROR &&
	    reply.token_len && reply.type != COAP_TYPE_RST) {

		int64_t proposed_ms = ack_unix_ms(reply.payload, reply.payload_len,
			IS_OPTION(&reply, COAP_OPTION_BLOCK2) && reply.block2_num == 0 && reply.block2_more);
		for (lwm2m_transaction_t *t = ctx->transactionList; t; t = t->next) {
			const coap_packet_t *request = t->message;
			if (t->peerH != peer || !request || request->code != COAP_POST ||
			    request->token_len != reply.token_len ||
			    memcmp(request->token, reply.token, reply.token_len) ||
			    (reply.type == COAP_TYPE_ACK && t->mID != reply.mid)) continue;

			/* Serialization frees request URI option nodes. Validate its saved
			 * wire buffer instead of inspecting those released nodes. */
			if (proposed_ms < 0 || !t->buffer || t->buffer_len > sizeof(copy)) continue;
			coap_packet_t sent = {0};
			memcpy(copy, t->buffer, t->buffer_len);
			if (coap_parse_message(&sent, copy, t->buffer_len) == COAP_NO_ERROR &&
			    sent.uri_path && sent.uri_path->len == 2 &&
			    memcmp(sent.uri_path->data, "rd", 2) == 0)
				unix_ms = proposed_ms + estimated_downlink_ms(peer, &reply);
			coap_free_header(&sent);
			break;
		}
	}
	coap_free_header(&reply);
	return unix_ms;
}
/* One configured gateway/context. Only timestamp metadata is retained; Wakaama
 * owns Block2 transfers and determines whether registration was accepted. */
static struct {
	lwm2m_context_t *ctx;
	void *peer;
	int64_t unix_ms, received_at;
} pending;
static void clear_pending(void) { memset(&pending, 0, sizeof(pending)); }
void halow_time_sync_reset(void)
{
	clear_pending(); memset(&tx, 0, sizeof(tx));
	memset(rtt_samples, 0, sizeof(rtt_samples));
	rtt_sum = rtt_count = rtt_cursor = 0;
}
void halow_time_sync_packet(lwm2m_context_t *ctx, uint8_t *packet, size_t length, void *peer)
{
	int64_t now = k_uptime_get();
	int64_t unix_ms = registration_time(ctx, packet, length, peer);
	if (unix_ms >= 0) {
		pending.ctx = ctx; pending.peer = peer;
		pending.unix_ms = unix_ms; pending.received_at = now;
	}
	lwm2m_handle_packet(ctx, packet, length, peer);
	if (pending.ctx != ctx || pending.peer != peer) return;
	if (k_uptime_get() - pending.received_at > 60000) {
		clear_pending(); return;
	}
	for (lwm2m_server_t *s = ctx->serverList; s; s = s->next) {
		if (s->sessionH != peer) continue;
		if (s->status == STATE_REGISTERED) {
			halow_device_sync_clock(ctx, pending.unix_ms + k_uptime_get() - pending.received_at);
			clear_pending();
		} else if (s->status != STATE_REG_PENDING && s->status != STATE_REG_UPDATE_PENDING) {
			clear_pending();
		}
		break;
	}
}

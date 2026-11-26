/* SPDX-License-Identifier: Apache-2.0
 * Bounded protobuf wire codec for Espressif's public provisioning schemas.
 * No heap, generated protobuf runtime, or platform dependency. */
#include "prov_protocol.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

struct view { const uint8_t *p; size_t n; };
struct writer { uint8_t *p; size_t n, cap; bool error; };

static int varint(struct view *v, uint64_t *out)
{
	uint64_t x = 0;
	for (unsigned int i = 0; i < 10; ++i) {
		if (!v->n) return -EBADMSG;
		uint8_t b = *v->p++; v->n--;
		if (i == 9 && b > 1) return -EBADMSG;
		x |= (uint64_t)(b & 127) << (7 * i);
		if (!(b & 128)) { *out = x; return 0; }
	}
	return -EBADMSG;
}

/* Validate the complete envelope, not just the field being sought. */
static int field(struct view v, uint32_t id, unsigned int type,
		 uint64_t *number, struct view *bytes)
{
	bool found = false;
	while (v.n) {
		uint64_t key, x = 0;
		struct view b = {0};
		if (varint(&v, &key) || !(key >> 3) || key >> 3 > 0x1fffffff) return -EBADMSG;
		unsigned int wire = key & 7;
		if (wire == 0) {
			if (varint(&v, &x)) return -EBADMSG;
		} else if (wire == 2) {
			if (varint(&v, &x) || x > v.n) return -EBADMSG;
			b = (struct view){v.p, (size_t)x}; v.p += x; v.n -= x;
		} else if (wire == 1 || wire == 5) {
			size_t length = wire == 1 ? 8 : 4;
			if (length > v.n) return -EBADMSG;
			v.p += length; v.n -= length;
		} else return -EBADMSG;
		if (key >> 3 == id) {
			if (wire != type || found) return -EBADMSG;
			found = true;
			if (number) *number = x;
			if (bytes) *bytes = b;
		}
	}
	return found ? 1 : 0;
}

static int integer(struct view v, uint32_t id, uint64_t *x)
{
	*x = 0;
	return field(v, id, 0, x, NULL);
}
static void put(struct writer *w, const void *p, size_t n)
{
	if (n > w->cap - w->n) { w->error = true; return; }
	if (n) memcpy(w->p + w->n, p, n);
	w->n += n;
}
static void vi(struct writer *w, uint64_t x)
{
	do { uint8_t b = (x & 127) | (x >= 128 ? 128 : 0); put(w, &b, 1); x >>= 7; } while (x);
}
static void num(struct writer *w, uint32_t id, uint64_t x)
{
	vi(w, (uint64_t)id << 3); vi(w, x);
}
static void blob(struct writer *w, uint32_t id, const void *p, size_t n)
{
	vi(w, ((uint64_t)id << 3) | 2); vi(w, n); put(w, p, n);
}
static int finish(struct writer *w) { return w->error ? -EMSGSIZE : (int)w->n; }

void prov_protocol_reset(struct prov_protocol *p)
{
	if (p->ops && p->ops->reset) p->ops->reset(p->user);
	p->state = 0;
	memset(p->client, 0, sizeof(p->client));
	memset(p->device, 0, sizeof(p->device));
}

static int session(struct prov_protocol *p, struct view req, struct writer *out)
{
	uint64_t version, msg;
	struct view sec = {0}, inner = {0}, data = {0};
	uint8_t payload[96], envelope[112], iv[16];
	struct writer a = {payload, 0, sizeof(payload), false};
	struct writer b = {envelope, 0, sizeof(envelope), false};
	if (integer(req, 2, &version) < 0 || version != 1 ||
	    field(req, 11, 2, NULL, &sec) != 1 || integer(sec, 1, &msg) < 0 ||
	    (msg != 0 && msg != 2) || field(sec, 20 + msg, 2, NULL, &inner) != 1 ||
	    field(inner, msg == 0 ? 1 : 2, 2, NULL, &data) != 1 || data.n != 32) return -EBADMSG;
	if (msg == 0) {
		prov_protocol_reset(p);
		memcpy(p->client, data.p, 32);
		if (p->ops->begin(p->user, p->client, p->device, iv)) return -EACCES;
		blob(&a, 2, p->device, 32); blob(&a, 3, iv, 16);
		p->state = 1;
	} else {
		if (p->state != 1) return -EACCES;
		uint8_t verify[32]; memcpy(verify, data.p, 32);
		if (p->ops->crypt(p->user, verify, 32)) return -EACCES;
		uint8_t diff = 0;
		for (size_t i = 0; i < 32; ++i) diff |= verify[i] ^ p->device[i];
		if (diff) { prov_protocol_reset(p); return -EACCES; }
		memcpy(verify, p->client, 32);
		if (p->ops->crypt(p->user, verify, 32)) return -EACCES;
		blob(&a, 3, verify, 32); p->state = 2;
	}
	if (a.error) return -EMSGSIZE;
	num(&b, 1, msg + 1); blob(&b, 21 + msg, a.p, a.n);
	if (b.error) return -EMSGSIZE;
	num(out, 2, 1); blob(out, 11, b.p, b.n);
	return finish(out);
}

static int config(struct prov_protocol *p, struct view req, struct writer *out)
{
	uint64_t msg;
	struct view inner = {0};
	uint8_t payload[192], connected[128];
	struct writer a = {payload, 0, sizeof(payload), false};
	if (integer(req, 1, &msg) < 0 || (msg != 0 && msg != 2 && msg != 4) ||
	    field(req, 10 + msg, 2, NULL, &inner) != 1) return -EBADMSG;
	int rc = 0;
	if (msg == 0) {
		struct prov_status status = {0}; p->ops->status(p->user, &status);
		num(&a, 2, status.state);
		if (status.state == 0) {
			struct writer c = {connected, 0, sizeof(connected), false};
			blob(&c, 1, status.ip, strlen(status.ip)); num(&c, 2, status.ap.auth);
			blob(&c, 3, status.ap.ssid, status.ap.ssid_len);
			blob(&c, 4, status.ap.bssid, 6); num(&c, 5, status.ap.channel);
			if (c.error) return -EMSGSIZE;
			blob(&a, 11, c.p, c.n);
		} else if (status.state == 3) num(&a, 10, status.fail_reason);
	} else if (msg == 2) {
		struct view ssid = {0}, pass = {0};
		if (field(inner, 1, 2, NULL, &ssid) != 1 ||
		    field(inner, 2, 2, NULL, &pass) < 0 || ssid.n < 1 || ssid.n > 32 || pass.n > 63 ||
		    memchr(ssid.p, 0, ssid.n) || (pass.n && memchr(pass.p, 0, pass.n))) rc = -EINVAL;
		else rc = p->ops->set(p->user, ssid.p, ssid.n, pass.p, pass.n);
	} else rc = p->ops->apply(p->user);
	if (rc) num(&a, 1, rc == -EINVAL ? 4 : 5);
	if (a.error) return -EMSGSIZE;
	num(out, 1, msg + 1); blob(out, 11 + msg, a.p, a.n);
	return finish(out);
}

static int scan(struct prov_protocol *p, struct view req, struct writer *out)
{
	uint64_t msg, start = 0, count = 0, passive = 0, blocking = 0;
	struct view inner = {0}; uint8_t payload[PROV_PACKET_MAX - 16];
	struct writer a = {payload, 0, sizeof(payload), false};
	if (integer(req, 1, &msg) < 0 || (msg != 0 && msg != 2 && msg != 4) ||
	    field(req, 10 + msg, 2, NULL, &inner) != 1) return -EBADMSG;
	int rc = 0;
	if (msg == 0) {
		if (integer(inner, 1, &blocking) < 0 || integer(inner, 2, &passive) < 0) return -EBADMSG;
		rc = p->ops->scan(p->user, passive != 0, blocking != 0);
	} else if (msg == 2) {
		bool done; uint32_t total;
		p->ops->scan_status(p->user, &done, &total);
		num(&a, 1, done); num(&a, 2, total);
	} else {
		if (integer(inner, 1, &start) < 0 || integer(inner, 2, &count) < 0 ||
		    start > UINT32_MAX || count > UINT32_MAX) return -EINVAL;
		for (uint32_t i = 0; i < count && i < PROV_SCAN_MAX && start + i <= UINT32_MAX; ++i) {
			struct prov_ap ap; uint8_t entry[96];
			struct writer e = {entry, 0, sizeof(entry), false};
			if (p->ops->scan_result(p->user, start + i, &ap)) break;
			if (ap.ssid_len > sizeof(ap.ssid)) return -EINVAL;
			blob(&e, 1, ap.ssid, ap.ssid_len); num(&e, 2, ap.channel);
			num(&e, 3, (uint64_t)(int64_t)ap.rssi); blob(&e, 4, ap.bssid, 6);
			num(&e, 5, ap.auth);
			if (e.error) return -EMSGSIZE;
			blob(&a, 1, e.p, e.n);
		}
	}
	if (a.error) return -EMSGSIZE;
	num(out, 1, msg + 1); if (rc) num(out, 2, 5);
	blob(out, 11 + msg, a.p, a.n);
	return finish(out);
}

int prov_protocol_exchange(struct prov_protocol *p, enum prov_endpoint endpoint,
			   uint8_t *request, size_t length, uint8_t *response, size_t capacity)
{
	if (!p || !p->ops || !response || capacity > PROV_PACKET_MAX ||
	    length > PROV_PACKET_MAX || (!request && length)) return -EINVAL;
	struct writer w = {response, 0, capacity, false};
	if (endpoint == PROV_VERSION) {
		const char version[] = "{\"prov\":{\"ver\":\"v1.1\",\"sec_ver\":1,\"sec_patch_ver\":0,\"cap\":[\"wifi_scan\"]},\"ez-dev54\":{\"ver\":\"1\"}}";
		put(&w, version, sizeof(version) - 1); return finish(&w);
	}
	int rc;
	if (endpoint == PROV_SESSION) rc = session(p, (struct view){request, length}, &w);
	else {
		if (p->state != 2) return -EACCES;
		if (endpoint != PROV_CONFIG && endpoint != PROV_SCAN && endpoint != PROV_MQTT_CONFIG) return -EINVAL;
		rc = p->ops->crypt(p->user, request, length);
		if (rc) { prov_protocol_reset(p); return -EACCES; }
		if (endpoint == PROV_MQTT_CONFIG) {
			int saved = p->ops->mqtt_config ? p->ops->mqtt_config(p->user, request, length) : -ENOTSUP;
			const char *reply = saved ? "{\"ok\":false,\"persisted\":false}" : "{\"ok\":true,\"persisted\":true}";
			put(&w, reply, strlen(reply)); rc = finish(&w);
		} else rc = endpoint == PROV_CONFIG ? config(p, (struct view){request, length}, &w) :
			scan(p, (struct view){request, length}, &w);
		if (rc >= 0 && p->ops->crypt(p->user, response, rc)) rc = -EACCES;
	}
	if (rc < 0) prov_protocol_reset(p);
	return rc;
}

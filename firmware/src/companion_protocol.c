/* SPDX-License-Identifier: Apache-2.0 */
#include "companion_protocol.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>
struct reader { const uint8_t *p; size_t n; };
struct writer { uint8_t *p; size_t n, cap; bool failed; };
static int number(struct reader *r, uint64_t *value)
{
	*value = 0;
	for (unsigned i = 0; i < 10; ++i) {
		if (!r->n) return -EBADMSG;
		uint8_t b = *r->p++; --r->n;
		if (i == 9 && b > 1) return -EBADMSG;
		*value |= (uint64_t)(b & 127) << (7 * i);
		if (!(b & 128)) return 0;
	}
	return -EBADMSG;
}
static int next(struct reader *r, uint32_t *tag, uint64_t *value, struct reader *bytes)
{
	uint64_t key;
	if (number(r, &key) || !(key >> 3) || key >> 3 > 0x1fffffff) return -EBADMSG;
	*tag = key >> 3; *value = 0; *bytes = (struct reader){0};
	switch (key & 7) {
	case 0: return number(r, value);
	case 1: if (r->n < 8) return -EBADMSG; r->p += 8; r->n -= 8; return 1;
	case 2:
		if (number(r, value) || *value > r->n) return -EBADMSG;
		*bytes = (struct reader){r->p, *value}; r->p += *value; r->n -= *value; return 2;
	case 5: if (r->n < 4) return -EBADMSG; r->p += 4; r->n -= 4; return 5;
	default: return -EBADMSG;
	}
}
static void put(struct writer *w, uint64_t value)
{
	do {
		if (w->n >= w->cap) { w->failed = true; return; }
		uint8_t b = value & 127; value >>= 7;
		w->p[w->n++] = b | (value ? 128 : 0);
	} while (value);
}
static void integer(struct writer *w, uint32_t tag, uint64_t value)
{
	put(w, (uint64_t)tag << 3); put(w, value);
}
static void blob(struct writer *w, uint32_t tag, const void *data, size_t size)
{
	put(w, ((uint64_t)tag << 3) | 2); put(w, size);
	if (w->failed || size > w->cap - w->n) { w->failed = true; return; }
	if (size) memcpy(w->p + w->n, data, size);
	w->n += size;
}
static void text(struct writer *w, uint32_t tag, const char *value)
{
	if (value && *value) blob(w, tag, value, strlen(value));
}
int companion_protocol_exchange(const struct companion_identity *id, const uint8_t *request,
				size_t length, uint8_t *response, size_t capacity)
{
	if (!id || !request || !length || length > COMPANION_PACKET_MAX || !response) return -EINVAL;
	struct reader input = {request, length}, body = {0};
	uint64_t from = 0, to = 0, operation = 0, value;
	uint32_t tag, kind = 0;
	while (input.n) {
		struct reader bytes;
		int wire = next(&input, &tag, &value, &bytes);
		if (wire < 0) return wire;
		if (tag == 1 || tag == 2 || tag == 3) {
			/* Scalar identity/operation must use the varint wire type. */
			if (wire || bytes.p) return -EBADMSG;
			if (tag == 1) from = value;
			if (tag == 2) to = value;
			if (tag == 3) operation = value;
		}
		if (tag >= 100 && tag <= 107) {
			if (kind || wire != 2) return -EBADMSG;
			kind = tag; body = bytes;
		}
	}
	if (operation != 1 || (to && to != id->node)) return -EACCES;
	if (kind != 102 && kind != 103 && kind != 104) return -ENOTSUP;
	uint64_t action = 0;
	while (body.n) {
		struct reader bytes;
		int wire = next(&body, &tag, &value, &bytes);
		if (wire < 0) return wire;
		if (kind == 104 && tag == 1) {
			if (wire) return -EBADMSG;
			action = value;
		}
	}
	if (kind == 104 && action != 1) return -EACCES;
	uint8_t payload[256];
	struct writer fields = {payload, 0, sizeof(payload), false};
	if (kind == 104) {
		integer(&fields, 1, 3); /* DEVICE_SETTINGS_REPORT */
		text(&fields, 5, id->name);
		integer(&fields, 24, 5); /* SENSOR, not a mesh relay */
		integer(&fields, 25, 0); /* Awake for this companion connection. */
		integer(&fields, 28, id->gps != 0);
		text(&fields, 29, id->icon);
	} else {
		integer(&fields, 1, 1); /* HaLow hardware present. */
		/* stack_initialized/mesh_mode/link_up/route_ready remain false:
		 * unprovisioned station mode must not claim an active mesh. */
		integer(&fields, 11, id->node);
		text(&fields, 13, id->version);
		kind = 102;
	}
	struct writer output = {response, 0, capacity, false};
	integer(&output, 1, id->node); integer(&output, 2, from);
	integer(&output, 3, 2); integer(&output, 4, 2); /* RESPONSE / BLE */
	blob(&output, kind, fields.p, fields.n);
	return fields.failed || output.failed ? -ENOSPC : (int)output.n;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include "companion_protocol.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include "companion_codec.h"
#include "companion_mesh.h"
static companion_mesh_handler_fn mesh_handler;
void companion_protocol_set_mesh_handler(companion_mesh_handler_fn handler) { mesh_handler = handler; }
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
	if (mesh_handler) {
		const struct companion_request parsed = {from, to, operation, kind, body.p, body.n, request, length};
		int result = mesh_handler(id, &parsed, response, capacity);
		if (result != -ENOTSUP) return result;
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

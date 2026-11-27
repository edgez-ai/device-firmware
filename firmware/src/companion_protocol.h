/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_COMPANION_PROTOCOL_H
#define EZ_DEV54_COMPANION_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#define COMPANION_PACKET_MAX 512
struct companion_identity {
	uint64_t node;
	const char *name, *version, *icon;
	const char *country;
	int gps;
};
/* EdgeZ NetworkPacket control codec. Optional mesh operations are bound only
 * during an unprovisioned BLE companion session; Wi-Fi credentials are separate. */
int companion_protocol_exchange(const struct companion_identity *, const uint8_t *, size_t,
				uint8_t *, size_t);
#endif

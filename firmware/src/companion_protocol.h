/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_COMPANION_PROTOCOL_H
#define EZ_DEV54_COMPANION_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#define COMPANION_PACKET_MAX 512
struct companion_identity {
	uint64_t node;
	const char *name, *version, *icon;
	int gps;
};
/* EdgeZ NetworkPacket: INIT/status and read-only DeviceSettings. This local
 * station companion does not apply mesh profiles or provision Wi-Fi. */
int companion_protocol_exchange(const struct companion_identity *, const uint8_t *, size_t,
				uint8_t *, size_t);
#endif

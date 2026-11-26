/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_PROV_PROTOCOL_H
#define EZ_DEV54_PROV_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROV_PACKET_MAX 512
#define PROV_SCAN_MAX 8
enum prov_endpoint { PROV_SESSION, PROV_CONFIG, PROV_SCAN, PROV_VERSION, PROV_MQTT_CONFIG };
struct prov_ap {
	uint8_t ssid[32], ssid_len, bssid[6], auth;
	uint32_t channel;
	int32_t rssi;
};
struct prov_status {
	uint32_t state, fail_reason;
	char ip[16];
	struct prov_ap ap;
};
/* begin creates an ephemeral X25519 key, mixes SHA256(PoP) into the shared
 * secret and initializes AES-256-CTR with the returned random IV. crypt uses
 * ONE continuous stream for both directions, as required by Security 1. */
struct prov_ops {
	int (*begin)(void *, const uint8_t client[32], uint8_t device[32], uint8_t iv[16]);
	int (*crypt)(void *, uint8_t *, size_t);
	void (*reset)(void *);
	int (*set)(void *, const uint8_t *, size_t, const uint8_t *, size_t);
	int (*apply)(void *);
	void (*status)(void *, struct prov_status *);
	int (*scan)(void *, bool, bool);
	void (*scan_status)(void *, bool *, uint32_t *);
	int (*scan_result)(void *, uint32_t, struct prov_ap *);
	int (*mqtt_config)(void *, const uint8_t *, size_t);
};
struct prov_protocol {
	const struct prov_ops *ops;
	void *user;
	uint8_t state, client[32], device[32];
};
void prov_protocol_reset(struct prov_protocol *);
int prov_protocol_exchange(struct prov_protocol *, enum prov_endpoint,
			   uint8_t *request, size_t request_len,
			   uint8_t *response, size_t capacity);
#endif

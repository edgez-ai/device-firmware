/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "companion_protocol.h"
#include <stdbool.h>
struct companion_request {
 uint64_t from, to, operation;
 uint32_t kind;
 const uint8_t *body; size_t body_size;
 const uint8_t *packet; size_t packet_size;
};
struct companion_mesh_profile {
 char country[3], mesh_id[33], passphrase[64], name[65], icon[17];
 uint8_t public_key[32]; size_t public_key_size;
 uint64_t user_id_high, user_id_low;
 uint32_t frequency_khz, bandwidth_mhz, max_hop, beacon_seconds, marker, role;
 bool share_location; uint32_t latitude_bits, longitude_bits;
};
struct companion_mesh_status { bool initialized, linked, route_ready; };
struct companion_mesh_ops {
 int (*configure)(const struct companion_mesh_profile *profile);
 int (*send)(uint64_t destination, const uint8_t *packet, size_t size);
 void (*status)(struct companion_mesh_status *status);
};
/* Bind only for the unprovisioned companion lifetime. Mesh settings stay in
 * RAM; they never become a station Wi-Fi or factory profile. */
void companion_mesh_bind(const struct companion_mesh_ops *ops);
typedef int (*companion_mesh_handler_fn)(const struct companion_identity *, const struct companion_request *, uint8_t *, size_t);
void companion_protocol_set_mesh_handler(companion_mesh_handler_fn handler);
int companion_mesh_exchange(const struct companion_identity *, const struct companion_request *, uint8_t *, size_t);
int companion_mesh_beacon(const struct companion_identity *, const struct companion_mesh_profile *, uint8_t *, size_t);
/* Worker-owned snapshots can be notified without touching the BLE parser's profile. */
int companion_mesh_status_packet(const struct companion_identity *, const char *, const struct companion_mesh_status *, uint64_t, uint8_t *, size_t);

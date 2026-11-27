/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "companion_mesh.h"
uint32_t companion_mesh_platform_random(void);
typedef void (*companion_mesh_deliver_fn)(const uint8_t *packet,size_t size);
/* These lifecycle/TX calls belong to one platform worker. RF receive dispatch
 * is owned by the private radio adapter; delivered bytes remain opaque. */
int companion_mesh_radio_start(const struct companion_identity *identity,
                              const struct companion_mesh_profile *profile,
                              companion_mesh_deliver_fn deliver);
void companion_mesh_radio_stop(void);
void companion_mesh_radio_tick(uint32_t now_ms);
int companion_mesh_radio_send(uint64_t destination,const uint8_t *packet,size_t size);
void companion_mesh_radio_status(struct companion_mesh_status *status);

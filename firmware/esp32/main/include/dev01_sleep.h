/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "liblwm2m.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
lwm2m_object_t *dev01_sleep_object(void);
void dev01_sleep_request_begin(uint32_t peer, const uint8_t *packet, size_t size);
void dev01_sleep_request_end(void);
void dev01_sleep_step(lwm2m_context_t *ctx, bool online);

void dev01_sleep_boot(void);
void dev01_sleep_online(void);

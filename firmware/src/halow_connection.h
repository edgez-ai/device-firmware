/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_HALOW_CONNECTION_H
#define EZ_DEV54_HALOW_CONNECTION_H
#include "liblwm2m.h"
#include <stdbool.h>
enum halow_connection_action { HALOW_CONN_NONE, HALOW_CONN_UPDATE, HALOW_CONN_REGISTER, HALOW_CONN_RECONNECT };
extern lwm2m_object_t halow_connection;
/* Run exclusively on the Wakaama thread, including all context lifecycle hooks. */
void halow_connection_pause(void);
void halow_connection_wake(void);
void halow_connection_context_closed(void);
enum halow_connection_action halow_connection_step(lwm2m_context_t *ctx, bool online);
int halow_connection_wait_ms(void);
#endif

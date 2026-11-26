/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_PROV_SESSION_H
#define EZ_DEV54_PROV_SESSION_H
#include <stdbool.h>
/* Call while holding the connection/config locks. Closed/closing windows and
 * saved/factory or in-flight Wi-Fi profiles cannot acquire companion mode. */
static inline bool prov_session_can_claim(bool enabled, bool configured, bool matching_peer,
					 bool open, bool closing)
{
	return enabled && !configured && matching_peer && open && !closing;
}
static inline bool prov_session_provisioning_open(bool open, bool closing, bool companion)
{
	return open && !closing && !companion;
}
static inline bool prov_session_hold_awake(bool connected, bool committed, bool closing)
{
	return connected && !committed && !closing;
}
#endif

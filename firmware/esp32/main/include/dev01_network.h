/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
/* Addresses and mask are in network byte order. Prefer the DHCP router;
 * a local-only AP may omit it while still hosting DHCP and LwM2M. */
static inline uint32_t dev01_lwm2m_server(uint32_t ip, uint32_t mask,
                                          uint32_t router,
                                          uint32_t dhcp_server) {
  if (!ip)
    return 0;
  if (router)
    return router;
  if (!mask || !dhcp_server || dhcp_server == ip ||
      (ip & mask) != (dhcp_server & mask))
    return 0;
  uint32_t host = dhcp_server & ~mask;
  if (!host || host == ~mask)
    return 0;
  return dhcp_server;
}

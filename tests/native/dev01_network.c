/* SPDX-License-Identifier: Apache-2.0 */
#include "dev01_network.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#define IP(a, b, c, d)                                                         \
  htonl(((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))
int main(void) {
  uint32_t ip = IP(10, 83, 0, 181), mask = IP(255, 255, 255, 0),
           local = IP(10, 83, 0, 1);
  assert(dev01_lwm2m_server(ip, mask, IP(10, 83, 0, 254), local) ==
         IP(10, 83, 0, 254));
  assert(dev01_lwm2m_server(ip, mask, 0, local) == local);
  assert(!dev01_lwm2m_server(ip, mask, 0, IP(192, 168, 12, 136)));
  assert(!dev01_lwm2m_server(ip, mask, 0, 0));
  assert(!dev01_lwm2m_server(ip, mask, 0, ip));
  assert(!dev01_lwm2m_server(ip, mask, 0, IP(10, 83, 0, 255)));
  assert(!dev01_lwm2m_server(ip, mask, 0, IP(10, 83, 0, 0)));
  assert(!dev01_lwm2m_server(0, mask, local, local));
  assert(!dev01_lwm2m_server(ip, 0, 0, local));
  puts("DHCP router preference, local-only server fallback and invalid-address "
       "rejection passed");
}

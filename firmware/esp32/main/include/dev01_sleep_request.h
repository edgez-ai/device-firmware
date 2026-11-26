/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
struct dev01_sleep_request {
  bool valid;
  uint32_t peer;
  uint16_t mid;
  uint8_t token[8], token_length;
};
static inline struct dev01_sleep_request dev01_sleep_request_decode(
    uint32_t peer, const uint8_t *p, size_t n) {
  struct dev01_sleep_request key = {0};
  if (!p || n < 4 || (p[0] >> 6) != 1 || ((p[0] >> 4) & 3) > 1 || p[1] != 2)
    return key;
  unsigned len = p[0] & 15;
  if (len > sizeof(key.token) || n < 4 + len) return key;
  key.valid = true; key.peer = peer;
  key.mid = (uint16_t)((uint16_t)p[2] << 8) | p[3];
  key.token_length = len; memcpy(key.token, p + 4, len);
  return key;
}
static inline bool dev01_sleep_request_same(const struct dev01_sleep_request *a,
                                           const struct dev01_sleep_request *b) {
  return a->valid && b->valid && a->peer == b->peer && a->mid == b->mid &&
      a->token_length == b->token_length && a->token_length <= sizeof(a->token) &&
      !memcmp(a->token, b->token, a->token_length);
}

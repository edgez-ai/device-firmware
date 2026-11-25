/* SPDX-License-Identifier: Apache-2.0 */
#include "factory_record.h"
#include <errno.h>
#include <string.h>
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static uint16_t le16(const uint8_t *p) {
  return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}
static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t crc = UINT32_MAX;
  while (n--) {
    crc ^= *p++;
    for (unsigned int i = 0; i < 8; ++i)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
  }
  return ~crc;
}
bool ez_factory_has_wifi(const struct ez_factory_data *d) {
  return d && d->ssid[0] && d->password[0];
}
int ez_factory_hardware_parse(const uint8_t *page, size_t size, uint8_t *mode) {
  if (!page || !mode)
    return -EINVAL;
  if (size < EZ_FACTORY_HARDWARE_OFFSET + EZ_FACTORY_HARDWARE_SIZE)
    return -ENOENT;
  const uint8_t *p = page + EZ_FACTORY_HARDWARE_OFFSET;
  if (le32(p) == UINT32_MAX || le32(p) == 0)
    return -ENOENT;
  if (le32(p) != 0x57485a45U || le16(p + 4) != 1 || le16(p + 6) != 16 ||
      le32(p + 12) != crc32(p, 12))
    return -EBADMSG;
  if (p[8] > 3 || p[9] || p[10] || p[11])
    return -EINVAL;
  *mode = p[8];
  return 0;
}
int ez_factory_parse(const uint8_t *p, size_t size, struct ez_factory_data *d) {
  if (!p || !d || size < EZ_FACTORY_RECORD_SIZE)
    return -EINVAL;
  memset(d, 0, sizeof(*d));
  if (le32(p) != 0x455a4631U || le16(p + 4) != 2 ||
      le16(p + 6) != EZ_FACTORY_RECORD_SIZE || le32(p + 207) != crc32(p, 207))
    return -EBADMSG;
  if (!p[8] || !memchr(p + 8, 0, 33) || !memchr(p + 108, 0, 33) ||
      !memchr(p + 142, 0, 65) || p[105] < 'A' || p[105] > 'Z' || p[106] < 'A' ||
      p[106] > 'Z' || p[107] != 0 || !p[141])
    return -EINVAL;
  uint8_t pub = 0, key = 0;
  for (size_t i = 0; i < 32; ++i) {
    pub |= p[41 + i];
    key |= p[73 + i];
  }
  if (!pub || !key)
    return -EINVAL;
  memcpy(d->serial, p + 8, 33);
  memcpy(d->public_key, p + 41, 32);
  memcpy(d->private_key, p + 73, 32);
  memcpy(d->country, p + 105, 3);
  memcpy(d->ssid, p + 108, 33);
  d->channel = p[141];
  memcpy(d->password, p + 142, 65);
  return 0;
}

/* Optional EZPM extension; leave the tracker-compatible identity record intact.
 */
int ez_factory_motion_parse(const uint8_t *page, size_t size,
                            uint16_t *threshold, uint8_t *angle) {
  if (!page || !threshold || !angle)
    return -EINVAL;
  if (size < EZ_FACTORY_MOTION_OFFSET + EZ_FACTORY_MOTION_SIZE)
    return -ENOENT;
  const uint8_t *p = page + EZ_FACTORY_MOTION_OFFSET;
  if (le32(p) == UINT32_MAX || le32(p) == 0)
    return -ENOENT;
  if (le32(p) != 0x4d505a45U || le16(p + 4) != 2 || le16(p + 6) != 16 ||
      le32(p + 12) != crc32(p, 12))
    return -EBADMSG;
  uint16_t mg = le16(p + 8);
  if (mg < EZ_FACTORY_POSE_MIN_MG || mg > EZ_FACTORY_POSE_MAX_MG ||
      (p[10] != 50 && p[10] != 60 && p[10] != 70 && p[10] != 80) || p[11])
    return -EINVAL;
  *threshold = mg;
  *angle = p[10];
  return 0;
}

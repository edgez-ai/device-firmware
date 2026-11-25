/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_FACTORY_RECORD_H
#define EZ_DEV54_FACTORY_RECORD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define EZ_FACTORY_ADDRESS 0x165000U
#define EZ_FACTORY_PAGE_SIZE 4096U
#define EZ_FACTORY_RECORD_SIZE 211U
#define EZ_FACTORY_HARDWARE_OFFSET 256U
#define EZ_FACTORY_HARDWARE_SIZE 16U
#define EZ_FACTORY_MOTION_OFFSET 272U
#define EZ_FACTORY_MOTION_SIZE 16U
#define EZ_FACTORY_SHAKE_DEFAULT_MG 1000U
#define EZ_FACTORY_SHAKE_MIN_MG 500U
#define EZ_FACTORY_SHAKE_MAX_MG 4000U
/* Compatible with template-offgrid-tracker's CRC-protected v2 record. */
struct ez_factory_data {
  char serial[33], country[3], ssid[33], password[65];
  uint8_t public_key[32], private_key[32], channel;
};
int ez_factory_parse(const uint8_t *, size_t, struct ez_factory_data *);
bool ez_factory_has_wifi(const struct ez_factory_data *);
/* Optional CRC-protected extension: 0 base, 1 camera, 2 radar, 3 GPS.
 * A missing or invalid extension must never select an external peripheral. */
int ez_factory_hardware_parse(const uint8_t *, size_t, uint8_t *);
int ez_factory_motion_parse(const uint8_t *, size_t, uint16_t *);
#endif

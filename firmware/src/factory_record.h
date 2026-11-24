/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_CAM_FACTORY_RECORD_H
#define EZ_CAM_FACTORY_RECORD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define EZ_FACTORY_ADDRESS 0x165000U
#define EZ_FACTORY_PAGE_SIZE 4096U
#define EZ_FACTORY_RECORD_SIZE 211U
/* Compatible with template-offgrid-tracker's CRC-protected v2 record. */
struct ez_factory_data {
	char serial[33], country[3], ssid[33], password[65];
	uint8_t public_key[32], private_key[32], channel;
};
int ez_factory_parse(const uint8_t *, size_t, struct ez_factory_data *);
bool ez_factory_has_wifi(const struct ez_factory_data *);
#endif

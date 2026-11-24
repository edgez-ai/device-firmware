/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RADAR_COMPAT_ENDIAN_H
#define RADAR_COMPAT_ENDIAN_H
#include <stdint.h>
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN
#define htole16(v) ((uint16_t)(v))
#define htole32(v) ((uint32_t)(v))
#define htole64(v) ((uint64_t)(v))
#define le16toh(v) ((uint16_t)(v))
#define le32toh(v) ((uint32_t)(v))
#define le64toh(v) ((uint64_t)(v))
#define htobe16(v) __builtin_bswap16((uint16_t)(v))
#define htobe32(v) __builtin_bswap32((uint32_t)(v))
#define htobe64(v) __builtin_bswap64((uint64_t)(v))
#define be16toh(v) __builtin_bswap16((uint16_t)(v))
#define be32toh(v) __builtin_bswap32((uint32_t)(v))
#define be64toh(v) __builtin_bswap64((uint64_t)(v))
#endif

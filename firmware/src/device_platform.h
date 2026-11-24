/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_DEVICE_PLATFORM_H
#define EZ_DEV54_DEVICE_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>
const char *board_device_serial(void);
const char *board_device_software(void);
uint32_t board_memory_free_kb(void);
uint32_t board_memory_total_kb(void);
int board_device_reboot(bool factory_reset);
int board_device_error(void);
void board_device_reset_errors(void);
#endif

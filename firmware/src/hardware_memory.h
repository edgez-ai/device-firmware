/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "hardware_config.h"
#include <stddef.h>
#include <stdint.h>
#define HARDWARE_CAMERA_IMAGE_CAPACITY (64U * 1024U)
#define HARDWARE_CAMERA_HEADER_SIZE 5U
#define HARDWARE_CAMERA_UART_SIZE 1024U
#define HARDWARE_GPS_UART_SIZE 512U
/* Boot-lifetime workspace. Allocate before starting BLE, UARTs or workers.
 * Failure leaves common services available; peripheral initialization fails. */
int hardware_memory_init(void);
uint8_t *hardware_memory_uart(enum hardware_mode expected);
uint8_t *hardware_memory_image(void);
size_t hardware_memory_size(void);

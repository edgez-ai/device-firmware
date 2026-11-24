/* SPDX-License-Identifier: Apache-2.0 */
#include "hardware_memory.h"
#include "hardware_config.h"
#include <errno.h>
#include <stdlib.h>
#include <zephyr/kernel.h>

static uint8_t *workspace;
static size_t workspace_size;
static enum hardware_mode owner;
int hardware_memory_init(void)
{
    if (workspace) return 0;
    enum hardware_mode mode = hardware_mode_get();
    size_t size;
    switch (mode) {
    case HARDWARE_BASE: return 0;
    case HARDWARE_CAMERA:
        size = HARDWARE_CAMERA_HEADER_SIZE + HARDWARE_CAMERA_IMAGE_CAPACITY + HARDWARE_CAMERA_UART_SIZE;
        break;
    case HARDWARE_RADAR: size = CONFIG_RADAR_UART_RX_BUFFER_SIZE; break;
    case HARDWARE_GPS: size = HARDWARE_GPS_UART_SIZE; break;
    default: return -EINVAL;
    }
    /* Newlib uses remaining RAM for its heap. Moving the cache out of BSS
     * leaves that RAM available to common services in every other mode.
     * Do not consume the separate kernel heap reserved for the radio. */
    uint8_t *allocated = malloc(size);
    if (!allocated) return -ENOMEM;
    workspace = allocated;
    workspace_size = size;
    owner = mode;
    return 0;
}
uint8_t *hardware_memory_uart(enum hardware_mode expected)
{
    if (!workspace || owner != expected) return NULL;
    return owner == HARDWARE_CAMERA ? workspace + HARDWARE_CAMERA_HEADER_SIZE + HARDWARE_CAMERA_IMAGE_CAPACITY : workspace;
}
uint8_t *hardware_memory_image(void)
{
    return owner == HARDWARE_CAMERA ? workspace : NULL;
}
size_t hardware_memory_size(void) { return workspace_size; }

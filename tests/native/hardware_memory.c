#include "hardware_config.h"
#include "hardware_memory.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
static enum hardware_mode selected;
static unsigned calls;
static size_t allocated_size;
static bool fail;
static void *allocation;
enum hardware_mode hardware_mode_get(void) { return selected; }
static void *test_malloc(size_t size) {
    ++calls; allocated_size = size;
    if (fail) return NULL;
    allocation = malloc(size);
    return allocation;
}
#define malloc test_malloc
#include "hardware_memory_under_test.c"
#undef malloc
int main(int argc, char **argv) {
    assert(argc == 2);
    int scenario = atoi(argv[1]);
    selected = scenario == 4 ? HARDWARE_CAMERA : (enum hardware_mode)scenario;
    assert(!hardware_memory_uart(selected) && !hardware_memory_image() && !hardware_memory_size());
    if (scenario == 4) {
        fail = true;
        assert(hardware_memory_init() == -ENOMEM && calls == 1);
        assert(!hardware_memory_uart(selected) && !hardware_memory_image() && !hardware_memory_size());
        fail = false; calls = 0;
    }
    if (scenario == 5) {
        assert(hardware_memory_init() == -EINVAL && calls == 0); return 0;
    }
    assert(!hardware_memory_init());
    if (selected == HARDWARE_BASE) {
        assert(calls == 0 && hardware_memory_size() == 0);
        assert(!hardware_memory_uart(selected) && !hardware_memory_image());
    } else {
        size_t expected = selected == HARDWARE_CAMERA ? 65536+5+1024 : selected == HARDWARE_RADAR ? 1024 : 512;
        assert(!hardware_memory_uart((enum hardware_mode)((selected+1)%4)));
        assert(calls == 1 && allocated_size == expected && hardware_memory_size() == expected);
        assert(hardware_memory_uart(selected));
        if (selected == HARDWARE_CAMERA) {
            uint8_t *image = hardware_memory_image(), *rx = hardware_memory_uart(selected);
            assert(image && rx == image + 65536 + 5);
            image[0] = 1; image[65536+4] = 2; rx[0] = 3; rx[1023] = 4;
            assert(image[65536+4] == 2 && rx[0] == 3);
        } else {
            assert(!hardware_memory_image());
            hardware_memory_uart(selected)[expected-1] = 5;
        }
        assert(!hardware_memory_init() && calls == 1); /* stable across capture/power cycles */
    }
    free(allocation);
    puts("Selected workspace allocation, buffer boundaries, lifetime and failure recovery passed");
    return 0;
}

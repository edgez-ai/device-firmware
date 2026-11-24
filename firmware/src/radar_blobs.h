/*
 * Data embedded at build time (generated into the build directory by
 * scripts/embed_blob.py, see zephyr/CMakeLists.txt):
 *  - the sensor configuration that is sent to the radar after every reset.
 */
#ifndef RADAR_BLOBS_H
#define RADAR_BLOBS_H

#include <stddef.h>
#include <stdint.h>

extern const uint8_t radar_config[];
extern const size_t radar_config_size;

#endif /* RADAR_BLOBS_H */

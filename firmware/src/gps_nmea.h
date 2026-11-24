/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_CAM_GPS_NMEA_H
#define EZ_CAM_GPS_NMEA_H
#include <stdbool.h>
#include <stdint.h>
struct gps_fix { double latitude, longitude; int64_t timestamp; };
/* Checksum-valid active GNRMC/GPRMC with valid WGS84 coordinates and UTC date. */
bool gps_nmea_fix(const char *line, struct gps_fix *fix);
#endif

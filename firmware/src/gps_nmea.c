/* SPDX-License-Identifier: Apache-2.0 */
#include "gps_nmea.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static bool coordinate(const char *text, const char *direction, int digits, double *out)
{
    size_t n = strlen(text);
    if (n < (size_t)digits + 2 || text[n - 1] == '.' || strlen(direction) != 1) return false;
    for (size_t i = 0; i < n; ++i) {
        if (text[i] == '.' && i == (size_t)digits + 2) continue;
        if (!isdigit((unsigned char)text[i])) return false;
    }
    char degrees[4] = {0}; memcpy(degrees, text, digits);
    double d = strtod(degrees, NULL), m = strtod(text + digits, NULL);
    double limit = digits == 2 ? 90 : 180;
    if (!isfinite(m) || m >= 60 || d > limit || (d == limit && m != 0)) return false;
    if (digits == 2 && direction[0] != 'N' && direction[0] != 'S') return false;
    if (digits == 3 && direction[0] != 'E' && direction[0] != 'W') return false;
    *out = d + m / 60;
    if (direction[0] == 'S' || direction[0] == 'W') *out = -*out;
    return true;
}
static int pair(const char *s) { return (s[0] - '0') * 10 + s[1] - '0'; }
static bool utc(const char *date, const char *time, int64_t *epoch)
{
    size_t n = strlen(time);
    if (strlen(date) != 6 || n < 6 || (n > 6 && (time[6] != '.' || n == 7))) return false;
    for (int i = 0; i < 6; ++i)
        if (!isdigit((unsigned char)date[i]) || !isdigit((unsigned char)time[i])) return false;
    for (size_t i = 7; i < n; ++i) if (!isdigit((unsigned char)time[i])) return false;
    int year = pair(date + 4); year += year >= 80 ? 1900 : 2000;
    int month = pair(date + 2), day = pair(date), hour = pair(time), minute = pair(time + 2), second = pair(time + 4);
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (month < 1 || month > 12 || day < 1 || day > days[month - 1] + (month == 2 && leap) ||
        hour > 23 || minute > 59 || second > 59) return false;
    int64_t total = 0;
    for (int y = 1970; y < year; ++y) total += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (int m = 1; m < month; ++m) total += days[m - 1] + (m == 2 && leap);
    *epoch = ((total + day - 1) * 24 + hour) * 3600 + minute * 60 + second;
    return true;
}
bool gps_nmea_fix(const char *line, struct gps_fix *fix)
{
    if (!line || !fix || line[0] != '$') return false;
    size_t n = strlen(line);
    if (n >= 128) return false;
    const char *star = strchr(line, '*');
    if (!star || strlen(star) < 3 || hex(star[1]) < 0 || hex(star[2]) < 0) return false;
    for (const char *p = star + 3; *p; ++p) if (*p != '\r' && *p != '\n') return false;
    unsigned int checksum = 0;
    for (const char *p = line + 1; p < star; ++p) checksum ^= (unsigned char)*p;
    if (checksum != (unsigned int)(hex(star[1]) * 16 + hex(star[2]))) return false;
    char buffer[128]; memcpy(buffer, line, star - line); buffer[star - line] = 0;
    char *fields[16] = {buffer}; unsigned int count = 1;
    for (char *p = buffer; *p; ++p) if (*p == ',') {
        *p = 0;
        if (count == 16) return false;
        fields[count++] = p + 1;
    }
    if (count < 10 || (strcmp(fields[0], "$GNRMC") && strcmp(fields[0], "$GPRMC")) ||
        strcmp(fields[2], "A")) return false;
    struct gps_fix next;
    if (!coordinate(fields[3], fields[4], 2, &next.latitude) ||
        !coordinate(fields[5], fields[6], 3, &next.longitude) ||
        !utc(fields[9], fields[1], &next.timestamp)) return false;
    *fix = next;
    return true;
}

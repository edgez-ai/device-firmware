/* SPDX-License-Identifier: Apache-2.0 */
#include "imu_wake.h"
#include <errno.h>
#include <string.h>
#include <zephyr/settings/settings.h>
static struct imu_wake_config config = {IMU_WAKE_DISABLED, 500, 60};
static int load(const char *name, size_t length, settings_read_cb read, void *arg)
{
    if (strcmp(name, "config")) return -ENOENT;
    uint8_t data[8];
    if (length != sizeof(data) || read(arg, data, sizeof(data)) != (ssize_t)sizeof(data) ||
        memcmp(data, "EZI\1", 4)) return -EINVAL;
    struct imu_wake_config parsed = {data[4], (uint16_t)data[6] | (uint16_t)data[7]<<8, data[5]};
    if (!imu_wake_config_valid(&parsed)) return -EINVAL;
    config = parsed;
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(ezdev54_imu_wake, "ezwake", NULL, load, NULL, NULL);
int imu_wake_config_init(void)
{
    int rc = settings_subsys_init();
    if (!rc) rc = settings_load_subtree("ezwake");
    return rc;
}
struct imu_wake_config imu_wake_config_get(void) { return config; }
int imu_wake_config_set(const struct imu_wake_config *next)
{
    if (!imu_wake_config_valid(next)) return -EINVAL;
    uint8_t data[8] = {'E','Z','I',1,next->mode,next->angle_deg,
                       (uint8_t)next->motion_mg,(uint8_t)(next->motion_mg>>8)};
    int rc = settings_save_one("ezwake/config", data, sizeof(data));
    if (!rc) config = *next;
    return rc;
}

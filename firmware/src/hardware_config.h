/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_HARDWARE_CONFIG_H
#define EZ_DEV54_HARDWARE_CONFIG_H
#include <stdbool.h>
enum hardware_mode { HARDWARE_BASE, HARDWARE_CAMERA, HARDWARE_RADAR, HARDWARE_GPS };
int hardware_config_init(void);
enum hardware_mode hardware_mode_get(void);
int hardware_config_desired(void);
int hardware_config_factory(void);
int hardware_config_source(void); /* 0 default, 1 factory, 2 saved override. */
int hardware_config_error(void);
bool hardware_config_pending(void);
int hardware_config_set(int mode);
int hardware_config_clear(void);
const char *hardware_type_name(void);
#endif

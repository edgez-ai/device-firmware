/* SPDX-License-Identifier: Apache-2.0 */
#include "hardware_config.h"
#include "factory_data.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/reboot.h>

static enum hardware_mode active;
static int desired, factory = -1, last_error;
static bool override;
static const uint8_t header[] = {'E', 'Z', 'H', 'W', 1};
static int load(const char *name, size_t length, settings_read_cb read, void *arg)
{
    if (strcmp(name, "hardware")) return -ENOENT;
    uint8_t record[8];
    if (length != sizeof(record) || read(arg, record, sizeof(record)) != sizeof(record) ||
        memcmp(record, header, sizeof(header)) || record[5] > HARDWARE_GPS || record[6] || record[7]) {
        last_error = -EINVAL; return -EINVAL;
    }
    desired = record[5]; override = true;
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(ezdev54_hardware, "ezdev54", NULL, load, NULL, NULL);
static void reboot_handler(struct k_work *work)
{
    (void)work;
    sys_reboot(SYS_REBOOT_COLD);
}
K_WORK_DELAYABLE_DEFINE(hardware_reboot, reboot_handler);
int hardware_config_init(void)
{
    factory = ez_factory_hardware();
    if (factory < HARDWARE_BASE || factory > HARDWARE_GPS) factory = -1;
    desired = factory >= 0 ? factory : HARDWARE_BASE;
    override = false; last_error = 0;
    int rc = settings_subsys_init();
    if (!rc) rc = settings_load_subtree("ezdev54");
    if (rc) last_error = rc;
    active = (enum hardware_mode)desired;
    return last_error;
}
enum hardware_mode hardware_mode_get(void) { return active; }
int hardware_config_desired(void) { return desired; }
int hardware_config_factory(void) { return factory; }
int hardware_config_source(void) { return override ? 2 : factory >= 0 ? 1 : 0; }
int hardware_config_error(void) { return last_error; }
bool hardware_config_pending(void) { return desired != (int)active; }
static int apply_saved(int mode, bool saved)
{
    desired = mode; override = saved;
    if (hardware_config_pending()) {
        /* Persist first, then allow the successful LwM2M response to leave. */
        int rc = k_work_reschedule(&hardware_reboot, K_SECONDS(2));
        last_error = rc < 0 ? rc : 0;
    } else {
        (void)k_work_cancel_delayable(&hardware_reboot);
        last_error = 0;
    }
    return last_error;
}
int hardware_config_set(int mode)
{
    if (mode < HARDWARE_BASE || mode > HARDWARE_GPS) return -EINVAL;
    uint8_t record[8] = {'E', 'Z', 'H', 'W', 1, (uint8_t)mode, 0, 0};
    int rc = settings_save_one("ezdev54/hardware", record, sizeof(record));
    if (rc) { last_error = rc; return rc; }
    return apply_saved(mode, true);
}
int hardware_config_clear(void)
{
    int rc = settings_delete("ezdev54/hardware");
    if (rc && rc != -ENOENT) { last_error = rc; return rc; }
    return apply_saved(factory >= 0 ? factory : HARDWARE_BASE, false);
}
const char *hardware_type_name(void)
{
    switch (active) {
    case HARDWARE_CAMERA: return "VC0706 camera";
    case HARDWARE_RADAR: return "IWRL6432 radar";
    case HARDWARE_GPS: return "L76K GPS";
    default: return "Base (no UART device)";
    }
}

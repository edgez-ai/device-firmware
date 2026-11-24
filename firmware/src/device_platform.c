/* SPDX-License-Identifier: Apache-2.0 */
#include "device_platform.h"
#include "sensors_hw.h"
#include "ble_prov.h"
#include <errno.h>
#include <string.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/version.h>
#if defined(CONFIG_EZ_CAM_FACTORY_DATA)
#include "factory_data.h"
#endif

static char serial[33];
static atomic_t reboot_pending;
static int reboot_error;
const char *board_device_serial(void)
{
#if defined(CONFIG_EZ_CAM_FACTORY_DATA)
	const struct ez_factory_data *factory = ez_factory_get();
	if (factory) return factory->serial;
#endif
	if (!serial[0]) {
		uint8_t id[8];
		ssize_t count = hwinfo_get_device_id(id, sizeof(id));
		if (count <= 0) return "unavailable";
		char *p = serial;
		for (ssize_t i = 0; i < count; ++i) {
			static const char hex[] = "0123456789ABCDEF";
			*p++ = hex[id[i] >> 4]; *p++ = hex[id[i] & 15];
		}
		*p = 0;
	}
	return serial;
}
const char *board_device_software(void) { return "Zephyr " KERNEL_VERSION_STRING; }
uint32_t board_memory_total_kb(void)
{
	return DT_REG_SIZE(DT_NODELABEL(cpuapp_sram)) / 1000;
}
uint32_t board_memory_free_kb(void)
{
	struct sys_heap **heaps;
	uint32_t available = 0;
	unsigned int key = irq_lock();
	int count = sys_heap_array_get(&heaps);
	for (int i = 0; i < count; ++i) {
		struct sys_memory_stats stats;
		if (!sys_heap_runtime_stats_get(heaps[i], &stats)) available += stats.free_bytes;
	}
	irq_unlock(key);
	return available / 1000;
}
static void reboot_work_handler(struct k_work *work)
{
	(void)work;
	if (atomic_get(&reboot_pending) == 2) {
#if defined(CONFIG_EZ_CAM_BLE_PROV)
		reboot_error = ez_prov_forget_network();
		if (reboot_error) { atomic_clear(&reboot_pending); return; }
#endif
	}
	sys_reboot(SYS_REBOOT_COLD);
}
K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_handler);
int board_device_reboot(bool factory_reset)
{
	if (!atomic_cas(&reboot_pending, 0, factory_reset ? 2 : 1)) return -EBUSY;
	/* Allow the 2.04 response to be transmitted before resetting. */
	int rc = k_work_schedule(&reboot_work, K_MSEC(500));
	if (rc < 0) { atomic_clear(&reboot_pending); return rc; }
	return 0;
}
int board_device_error(void)
{
	return reboot_error ? 16 : (board_sensors_error() ? 8 : 0);
}
void board_device_reset_errors(void)
{
	reboot_error = 0;
	/* Sensor error conditions are re-evaluated from their last actual I/O;
	 * do not start hardware sampling merely to reset the error resource. */
}

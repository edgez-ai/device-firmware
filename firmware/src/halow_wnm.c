/* SPDX-License-Identifier: Apache-2.0 */
#include "halow_sleep.h"
#include <errno.h>
#include <mmwlan.h>
#include <zephyr/net/net_if.h>
#if defined(CONFIG_EZ_DEV54_WNM_PARK_PINS)
#include "halow_wnm_pins.h"
#endif

static struct net_if *suspended_iface;
int halow_wnm_enter(void)
{
    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) return -ENODEV;
    /* Gate IP/ARP/DHCP TX without bringing the interface or association down.
     * WNM negotiation uses the SDK management path, outside IP. */
    int rc = net_if_suspend(iface);
    if (rc) return rc;
    suspended_iface = iface;
    struct mmwlan_set_wnm_sleep_enabled_args args = MMWLAN_SET_WNM_SLEEP_ENABLED_ARGS_INIT;
    args.wnm_sleep_enabled = true;
    args.chip_powerdown_enabled = true;
    rc = mmwlan_set_wnm_sleep_enabled_ext(&args);
#if defined(CONFIG_EZ_DEV54_WNM_PARK_PINS)
    if (rc == MMWLAN_SUCCESS) {
        rc = halow_wnm_pins_park();
        if (rc) {
            /* Entry succeeded but host SPI suspend failed. Undo WNM rather
             * than reporting a failed entry while leaving the radio asleep. */
            args.wnm_sleep_enabled = false;
            (void)mmwlan_set_wnm_sleep_enabled_ext(&args);
        }
    }
#endif
    if (rc != MMWLAN_SUCCESS) {
        (void)net_if_resume(iface);
        suspended_iface = NULL;
    }
    return rc;
}
int halow_wnm_exit(void)
{
#if defined(CONFIG_EZ_DEV54_WNM_PARK_PINS)
    /* Never let SDK restoration access disconnected signal pins. */
    int restore_rc = halow_wnm_pins_restore();
#endif
    struct mmwlan_set_wnm_sleep_enabled_args args = MMWLAN_SET_WNM_SLEEP_ENABLED_ARGS_INIT;
    args.chip_powerdown_enabled = true;
    int rc;
#if defined(CONFIG_EZ_DEV54_WNM_PARK_PINS)
    rc = restore_rc ? restore_rc : mmwlan_set_wnm_sleep_enabled_ext(&args);
#else
    rc = mmwlan_set_wnm_sleep_enabled_ext(&args);
#endif
    /* SDK exits chip low power even when the AP exit exchange times out. */
    if (suspended_iface) {
        int resume_rc = net_if_resume(suspended_iface);
        suspended_iface = NULL;
        if (!rc && resume_rc) rc = resume_rc;
    }
    return rc;
}

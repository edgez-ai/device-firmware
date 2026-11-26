/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_BLE_PROV_H
#define EZ_DEV54_BLE_PROV_H
#include <stdbool.h>
struct net_if;
#if defined(CONFIG_EZ_DEV54_BLE_PROV)
int ez_prov_init(void);
const char *ez_prov_device_name(void);
int ez_prov_forget_network(void);
bool ez_prov_get_credentials(char ssid[33], char password[64]);
void ez_prov_wifi_failed(int status);
void ez_prov_wifi_bound(struct net_if *iface);
bool ez_prov_retry_allowed(void);
void ez_prov_button_changed(bool pressed);
#endif
#endif

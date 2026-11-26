/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_BLE_COMPANION_H
#define EZ_DEV54_BLE_COMPANION_H
struct bt_conn;
#if defined(CONFIG_EZ_DEV54_BLE_COMPANION)
int ez_companion_register(void);
void ez_companion_unregister(void);
void ez_companion_release_buffers(void);
void ez_companion_reset(struct bt_conn *conn);
#else
static inline int ez_companion_register(void) { return 0; }
static inline void ez_companion_unregister(void) {}
static inline void ez_companion_release_buffers(void) {}
static inline void ez_companion_reset(struct bt_conn *conn) { (void)conn; }
#endif
#endif

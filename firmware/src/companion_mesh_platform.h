/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#if defined(CONFIG_EZ_DEV54_BLE_COMPANION) || defined(ESP_PLATFORM)
void companion_mesh_session(bool connected);
#else
static inline void companion_mesh_session(bool connected) {(void)connected;}
#endif

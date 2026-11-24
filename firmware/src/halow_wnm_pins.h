/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_HALOW_WNM_PINS_H
#define EZ_DEV54_HALOW_WNM_PINS_H
/* Only after successful SDK chip powerdown; restore before asking SDK to wake. */
int halow_wnm_pins_park(void);
int halow_wnm_pins_restore(void);
#endif

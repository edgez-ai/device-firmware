/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "driver/gpio.h"
#include <stdbool.h>
/* Heltec HT-HC33 schematic V1.0.0: USER key is active-low;
 * GPIO19 drives SK6812 DIN, not a discrete LED. */
#define DEV01_BUTTON_GPIO GPIO_NUM_0
#define DEV01_LED_GPIO GPIO_NUM_19
void dev01_led_init(void);
void dev01_led_off(void);
void dev01_led_toggle(void);

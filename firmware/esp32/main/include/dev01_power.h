/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void dev01_power_init(void);
void dev01_power_start(void);
void dev01_power_veto(uint8_t id, bool set);
void dev01_power_radio_awake(bool awake);
int dev01_power_report(char *out, size_t size);

void dev01_power_restore_wake(void);
bool dev01_power_radio_quiet(void);

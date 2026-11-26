/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "edgez_esp_factory_data.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct {
  uint32_t magic;
  char ssid[33];
  char password[64];
} dev01_wifi_t;
extern edgez_esp_factory_identity_t dev01_identity;
const char *dev01_name(void);
const char *dev01_pop(void);
bool dev01_configured(void);
esp_err_t dev01_set_name(const char *name);
esp_err_t dev01_request_wifi(const dev01_wifi_t *wifi);
void dev01_wifi_status(unsigned *state, char ip[16]);
esp_err_t dev01_onboarding(void);
void dev01_lwm2m_start(void);
void dev01_reboot(bool forget_wifi);
const char *dev01_factory_country(void);

const char *dev01_model(void);

#include "prov_protocol.h"
esp_err_t dev01_radio_acquire(void);
void dev01_radio_release(void);
int dev01_scan_start(void *user, bool passive, bool blocking);
void dev01_scan_status(void *user, bool *done, uint32_t *count);
int dev01_scan_result(void *user, uint32_t index, struct prov_ap *ap);

bool dev01_companion_available(void);

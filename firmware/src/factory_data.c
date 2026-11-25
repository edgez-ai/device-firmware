/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/autoconf.h>
#if defined(CONFIG_EZ_DEV54_FACTORY_DATA)
#include "factory_data.h"
#include <errno.h>
#include <psa/crypto.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

static struct ez_factory_data cached;
static bool attempted;
static int load_result = -ENOENT;
static char pop[33], ble_name[15];
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(factory_partition)) == EZ_FACTORY_ADDRESS);

int ez_factory_init(void)
{
	if (attempted) return load_result;
	attempted = true;
	/* The radio's boot hook runs before settings/flash initialization. nRF54
	 * RRAM is memory mapped; parse once without requiring a flash driver. */
	uintptr_t address = DT_REG_ADDR(DT_NODELABEL(cpuapp_rram)) + EZ_FACTORY_ADDRESS;
	load_result = ez_factory_parse((const uint8_t *)address, EZ_FACTORY_RECORD_SIZE, &cached);
	return load_result;
}
const struct ez_factory_data *ez_factory_get(void)
{
	return ez_factory_init() == 0 ? &cached : NULL;
}

/* Strong override of the optional EdgeZ driver boot-time country hook.
 * Invalid/missing factory data returns an invalid domain: no RF start. */
const char *morsemicro_get_country_code(void);
const char *morsemicro_get_country_code(void)
{
	return ez_factory_init() == 0 ? cached.country : "00";
}

int ez_factory_crypto_init(void)
{
	/* Versioned KDF domain: keep existing device PoPs and QR labels valid. */
	const char label[] = "EZ-CAM01 BLE provisioning PoP v1";
	uint8_t digest[32]; size_t size;
	psa_key_id_t key = 0;
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	if (ez_factory_init()) return -ENOENT;
	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	psa_status_t rc = psa_import_key(&attr, cached.private_key, 32, &key);
	psa_reset_key_attributes(&attr);
	if (rc != PSA_SUCCESS) return -EIO;
	rc = psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), (const uint8_t *)label,
			     sizeof(label) - 1, digest, sizeof(digest), &size);
	(void)psa_destroy_key(key);
	if (rc != PSA_SUCCESS || size != 32) return -EIO;
	const char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < 16; ++i) { pop[2*i] = hex[digest[i] >> 4]; pop[2*i+1] = hex[digest[i] & 15]; }
	pop[32] = 0;
	rc = psa_hash_compute(PSA_ALG_SHA_256, cached.public_key, 32, digest, sizeof(digest), &size);
	if (rc != PSA_SUCCESS || size != 32) return -EIO;
	memcpy(ble_name, "PROV_CAM", 8);
	for (size_t i = 0; i < 3; ++i) {
		ble_name[8+2*i] = hex[digest[i] >> 4]; ble_name[9+2*i] = hex[digest[i] & 15];
	}
	ble_name[14] = 0;
	volatile uint8_t *wipe = digest;
	for (size_t i = 0; i < sizeof(digest); ++i) wipe[i] = 0;
	return 0;
}
const char *ez_factory_pop(void) { return pop; }
const char *ez_factory_ble_name(void) { return ble_name; }
int ez_factory_hardware(void)
{
	if (ez_factory_init()) return -1;
	uintptr_t address = DT_REG_ADDR(DT_NODELABEL(cpuapp_rram)) + EZ_FACTORY_ADDRESS;
	uint8_t mode;
	return ez_factory_hardware_parse((const uint8_t *)address, EZ_FACTORY_PAGE_SIZE, &mode) == 0 ? mode : -1;
}
uint16_t ez_factory_shake_threshold_mg(void)
{
    uint16_t threshold = EZ_FACTORY_SHAKE_DEFAULT_MG;
    if (ez_factory_init()) return threshold;
    uintptr_t address = DT_REG_ADDR(DT_NODELABEL(cpuapp_rram)) + EZ_FACTORY_ADDRESS;
    uint16_t saved;
    if (!ez_factory_motion_parse((const uint8_t *)address, EZ_FACTORY_PAGE_SIZE, &saved))
        threshold = saved;
    return threshold;
}
#endif

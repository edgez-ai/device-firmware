/* SPDX-License-Identifier: Apache-2.0
 * ESP-IDF-compatible provisioning on Zephyr. Settings/ZMS and bt_disable
 * lifecycle follow template-offgrid-tracker; the wire format is Protocomm. */
#include "ble_prov.h"
#if defined(CONFIG_EZ_CAM_BLE_PROV)
#include "prov_protocol.h"
#include "halow_sta.h"
#include "factory_data.h"
#include <errno.h>
#include <string.h>
#include <psa/crypto.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

struct credentials {
	uint32_t magic;
	uint8_t ssid_len, password_len;
	char ssid[33], password[64];
};
#define CONFIG_MAGIC 0x455a4301U
static struct credentials active, staged;
static bool have_active, have_staged, committed, factory_wifi;
static uint32_t config_generation;
static uint32_t wifi_state = 2, fail_reason;
static char ip_address[16];
static volatile int last_error, last_protocol_error, last_crypto_error;
/* This Zephyr application has no process teardown/destructors. Arm newlib's
 * exit path, pulled in by PSA platform support, still requires its CRT hook. */
void _fini(void);
__weak void _fini(void) {}
static K_MUTEX_DEFINE(config_lock);
static struct bt_conn *connection;
static struct k_spinlock connection_lock;
static K_MUTEX_DEFINE(protocol_lock);
static struct bt_conn *connection_ref(void)
{
	k_spinlock_key_t key = k_spin_lock(&connection_lock);
	struct bt_conn *c = connection ? bt_conn_ref(connection) : NULL;
	k_spin_unlock(&connection_lock, key);
	return c;
}
static bool bt_started;
static atomic_t window_open;
static atomic_t button_pressed;
static atomic_t reset_armed;
static uint8_t rx[PROV_PACKET_MAX], tx[PROV_PACKET_MAX];
static int tx_length;
static enum prov_endpoint tx_endpoint;
static struct net_mgmt_event_callback scan_cb;
static struct prov_ap scan_entries[PROV_SCAN_MAX];
static uint32_t scan_count;
static bool scan_finished = true;
static K_MUTEX_DEFINE(scan_lock);
static K_SEM_DEFINE(scan_done, 0, 1);

static void begin_window(struct k_work *work);
static void stop_window(struct k_work *work);
static void save_network(struct k_work *work);
static void held_reset(struct k_work *work);
static void advertise(struct k_work *work);
K_WORK_DEFINE(start_work, begin_window);
K_WORK_DEFINE(save_work, save_network);
K_WORK_DELAYABLE_DEFINE(stop_work, stop_window);
K_WORK_DELAYABLE_DEFINE(reset_work, held_reset);
K_WORK_DELAYABLE_DEFINE(advertise_work, advertise);

static void wipe(void *buffer, size_t size)
{
	volatile uint8_t *p = buffer;
	while (size--) *p++ = 0;
}

/* PSA's AES-CTR encrypt operation is also the decrypt operation (XOR).
 * One stream is retained across client proof, device proof and endpoint data. */
static psa_key_id_t aes_key;
static psa_cipher_operation_t cipher = PSA_CIPHER_OPERATION_INIT;
static void crypto_reset(void *user)
{
	ARG_UNUSED(user);
	(void)psa_cipher_abort(&cipher);
	if (aes_key) (void)psa_destroy_key(aes_key);
	aes_key = 0;
}
static int crypto_begin(void *user, const uint8_t client[32], uint8_t device[32], uint8_t iv[16])
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key = 0;
	uint8_t secret[32], digest[32]; size_t length;
	int rc = -EIO;
	ARG_UNUSED(user);
	crypto_reset(NULL);
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
	psa_set_key_bits(&attr, 255);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
	if (psa_generate_key(&attr, &key) != PSA_SUCCESS) goto done;
	if (psa_export_public_key(key, device, 32, &length) != PSA_SUCCESS || length != 32) goto done;
	if (psa_raw_key_agreement(PSA_ALG_ECDH, key, client, 32, secret, 32, &length) != PSA_SUCCESS || length != 32) goto done;
	if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)ez_factory_pop(),
		strlen(ez_factory_pop()), digest, sizeof(digest), &length) != PSA_SUCCESS || length != 32) goto done;
	for (size_t i = 0; i < 32; ++i) secret[i] ^= digest[i];
	if (psa_generate_random(iv, 16) != PSA_SUCCESS) goto done;
	psa_reset_key_attributes(&attr);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
	psa_set_key_algorithm(&attr, PSA_ALG_CTR);
	if (psa_import_key(&attr, secret, 32, &aes_key) != PSA_SUCCESS) goto done;
	if (psa_cipher_encrypt_setup(&cipher, aes_key, PSA_ALG_CTR) != PSA_SUCCESS ||
	    psa_cipher_set_iv(&cipher, iv, 16) != PSA_SUCCESS) goto done;
	rc = 0;
done:
	if (key) (void)psa_destroy_key(key);
	psa_reset_key_attributes(&attr);
	wipe(secret, sizeof(secret)); wipe(digest, sizeof(digest));
	if (rc) crypto_reset(NULL);
	return rc;
}
static int crypto_stream(void *user, uint8_t *buffer, size_t size)
{
	size_t length = 0;
	uint8_t output[PROV_PACKET_MAX + 16];
	ARG_UNUSED(user);
	if (size > PROV_PACKET_MAX) return -EMSGSIZE;
	/* Mbed TLS rejects in-place cipher_update for non-block-sized input,
	 * including short protobuf messages in CTR mode. Keep buffers distinct. */
	psa_status_t rc = psa_cipher_update(&cipher, buffer, size, output, sizeof(output), &length);
	last_crypto_error = rc;
	if (rc == PSA_SUCCESS && length == size) memcpy(buffer, output, size);
	wipe(output, sizeof(output));
	return rc == PSA_SUCCESS && length == size ? 0 : -EIO;
}

static bool valid_record(const struct credentials *c)
{
	return c->magic == CONFIG_MAGIC && c->ssid_len > 0 && c->ssid_len <= 32 &&
		c->password_len >= 8 && c->password_len <= 63 && !memchr(c->ssid, 0, c->ssid_len) &&
		!memchr(c->password, 0, c->password_len) &&
		c->ssid[c->ssid_len] == 0 && c->password[c->password_len] == 0;
}
static int load_network(const char *name, size_t length, settings_read_cb read, void *arg)
{
	struct credentials record;
	if (strcmp(name, "wifi")) return -ENOENT;
	if (length != sizeof(record)) return -EINVAL;
	if (read(arg, &record, sizeof(record)) != sizeof(record) || !valid_record(&record)) {
		wipe(&record, sizeof(record)); return -EINVAL;
	}
	if (!factory_wifi) { active = record; have_active = true; committed = true; }
	wipe(&record, sizeof(record));
	return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(ezcam, "ezcam", NULL, load_network, NULL, NULL);

bool ez_prov_get_credentials(char ssid[33], char password[64])
{
	k_mutex_lock(&config_lock, K_FOREVER);
	bool valid = have_active;
	if (valid) { memcpy(ssid, active.ssid, 33); memcpy(password, active.password, 64); }
	k_mutex_unlock(&config_lock);
	return valid;
}
bool ez_prov_retry_allowed(void)
{
	k_mutex_lock(&config_lock, K_FOREVER);
	bool allow = !atomic_get(&window_open) || committed;
	k_mutex_unlock(&config_lock);
	return allow;
}
static int set_network(void *user, const uint8_t *ssid, size_t ssid_len,
		       const uint8_t *password, size_t password_len)
{
	ARG_UNUSED(user);
	if (!ssid_len || ssid_len > 32 || password_len < 8 || password_len > 63 ||
	    memchr(ssid, 0, ssid_len) || memchr(password, 0, password_len)) return -EINVAL;
	if (factory_wifi) return -EACCES;
	k_mutex_lock(&config_lock, K_FOREVER);
	wipe(&staged, sizeof(staged));
	staged.magic = CONFIG_MAGIC;
	staged.ssid_len = ssid_len; staged.password_len = password_len;
	memcpy(staged.ssid, ssid, ssid_len);
	if (password_len) memcpy(staged.password, password, password_len);
	have_staged = true;
	k_mutex_unlock(&config_lock);
	return 0;
}
static int apply_network(void *user)
{
	ARG_UNUSED(user);
	k_mutex_lock(&config_lock, K_FOREVER);
	if (!have_staged) { k_mutex_unlock(&config_lock); return -EINVAL; }
	active = staged; have_active = true; committed = false;
	config_generation++; wifi_state = 1;
	wipe(&staged, sizeof(staged)); have_staged = false;
	k_mutex_unlock(&config_lock);
	return halow_sta_reconnect();
}
static void get_status(void *user, struct prov_status *status)
{
	ARG_UNUSED(user);
	k_mutex_lock(&config_lock, K_FOREVER);
	status->state = wifi_state; status->fail_reason = fail_reason;
	memcpy(status->ip, ip_address, sizeof(status->ip));
	status->ap.ssid_len = active.ssid_len;
	memcpy(status->ap.ssid, active.ssid, active.ssid_len);
	status->ap.auth = active.password_len ? 6 : 0;
	bool success = wifi_state == 0 && committed;
	k_mutex_unlock(&config_lock);
	if (success) (void)k_work_reschedule(&stop_work, K_SECONDS(CONFIG_EZ_CAM_PROV_SUCCESS_GRACE_SECONDS));
}
void ez_prov_wifi_failed(int status)
{
	k_mutex_lock(&config_lock, K_FOREVER);
	wifi_state = 3;
	fail_reason = status == -ETIMEDOUT || status == -ENETUNREACH ? 1 : 0;
	k_mutex_unlock(&config_lock);
}
void ez_prov_wifi_bound(struct net_if *iface)
{
	struct wifi_iface_status link = {0};
	int rc = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &link, sizeof(link));
	struct net_in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	k_mutex_lock(&config_lock, K_FOREVER);
	/* Do not persist a late DHCP event from the previous configuration. */
	if (rc || !addr || link.ssid_len != active.ssid_len ||
	    memcmp(link.ssid, active.ssid, active.ssid_len)) {
		k_mutex_unlock(&config_lock); return;
	}
	if (addr) (void)net_addr_ntop(AF_INET, addr, ip_address, sizeof(ip_address));
	bool needs_save = have_active && !committed;
	if (!needs_save) wifi_state = 0;
	k_mutex_unlock(&config_lock);
	if (needs_save) (void)k_work_submit(&save_work);
}
static void save_network(struct k_work *work)
{
	ARG_UNUSED(work);
	k_mutex_lock(&config_lock, K_FOREVER);
	struct credentials record = active; uint32_t generation = config_generation;
	k_mutex_unlock(&config_lock);
	int rc = settings_save_one("ezcam/wifi", &record, sizeof(record));
	wipe(&record, sizeof(record));
	k_mutex_lock(&config_lock, K_FOREVER);
	last_error = rc;
	if (generation == config_generation) {
		committed = rc == 0; wifi_state = rc == 0 ? 0 : 3;
	}
	k_mutex_unlock(&config_lock);
}

static void scan_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(iface);
	k_mutex_lock(&scan_lock, K_FOREVER);
	if (event == NET_EVENT_WIFI_SCAN_DONE) { scan_finished = true; k_sem_give(&scan_done); }
	else if (event == NET_EVENT_WIFI_SCAN_RESULT && cb->info && scan_count < PROV_SCAN_MAX) {
		const struct wifi_scan_result *r = cb->info;
		struct prov_ap *ap = &scan_entries[scan_count];
		if (r->ssid_length > sizeof(ap->ssid)) { k_mutex_unlock(&scan_lock); return; }
		memset(ap, 0, sizeof(*ap));
		memcpy(ap->ssid, r->ssid, r->ssid_length); ap->ssid_len = r->ssid_length;
		memcpy(ap->bssid, r->mac, sizeof(ap->bssid)); ap->channel = r->channel;
		ap->rssi = r->rssi; ap->auth = r->security == WIFI_SECURITY_TYPE_NONE ? 0 : 6;
		scan_count++;
	}
	k_mutex_unlock(&scan_lock);
}
static int start_scan(void *user, bool passive, bool blocking)
{
	ARG_UNUSED(user);
	struct net_if *iface = net_if_get_first_wifi();
	if (!iface) return -ENODEV;
	k_mutex_lock(&scan_lock, K_FOREVER);
	if (!scan_finished) { k_mutex_unlock(&scan_lock); return -EBUSY; }
	scan_count = 0; scan_finished = false; k_sem_reset(&scan_done);
	k_mutex_unlock(&scan_lock);
	struct wifi_scan_params params = { .scan_type = passive ? WIFI_SCAN_TYPE_PASSIVE : WIFI_SCAN_TYPE_ACTIVE };
	int rc = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &params, sizeof(params));
	if (rc) { k_mutex_lock(&scan_lock, K_FOREVER); scan_finished = true; k_mutex_unlock(&scan_lock); }
	if (!rc && blocking) rc = k_sem_take(&scan_done, K_SECONDS(8));
	return rc;
}
static void get_scan_status(void *user, bool *done, uint32_t *count)
{
	ARG_UNUSED(user); k_mutex_lock(&scan_lock, K_FOREVER);
	*done = scan_finished; *count = scan_count; k_mutex_unlock(&scan_lock);
}
static int get_scan_result(void *user, uint32_t index, struct prov_ap *ap)
{
	ARG_UNUSED(user); k_mutex_lock(&scan_lock, K_FOREVER);
	int rc = index < scan_count ? 0 : -ENOENT;
	if (!rc) *ap = scan_entries[index];
	k_mutex_unlock(&scan_lock); return rc;
}
static const struct prov_ops ops = {
	.begin = crypto_begin, .crypt = crypto_stream, .reset = crypto_reset,
	.set = set_network, .apply = apply_network, .status = get_status,
	.scan = start_scan, .scan_status = get_scan_status, .scan_result = get_scan_result,
};
static struct prov_protocol protocol = { .ops = &ops };

#ifdef CONFIG_EZ_CAM_PROV_TRACKER_UUID
#define SERVICE_UUID BT_UUID_128_ENCODE(0x0000fff0, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define ENDPOINT_UUID(id) BT_UUID_128_ENCODE(0x00000000 | (id), 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#else
#define SERVICE_UUID BT_UUID_128_ENCODE(0x1775244d, 0x6b43, 0x439b, 0x877c, 0x060f2d9bed07)
#define ENDPOINT_UUID(id) BT_UUID_128_ENCODE(0x17750000 | (id), 0x6b43, 0x439b, 0x877c, 0x060f2d9bed07)
#endif
static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(SERVICE_UUID);
static struct bt_uuid_128 session_uuid = BT_UUID_INIT_128(ENDPOINT_UUID(0xff51));
static struct bt_uuid_128 config_uuid = BT_UUID_INIT_128(ENDPOINT_UUID(0xff52));
static struct bt_uuid_128 scan_uuid = BT_UUID_INIT_128(ENDPOINT_UUID(0xff50));
static struct bt_uuid_128 version_uuid = BT_UUID_INIT_128(ENDPOINT_UUID(0xff53));
static const enum prov_endpoint endpoints[] = {PROV_SESSION, PROV_CONFIG, PROV_SCAN, PROV_VERSION};
static const char version[] = "{\"prov\":{\"ver\":\"v1.1\",\"sec_ver\":1,\"sec_patch_ver\":0,\"cap\":[\"wifi_scan\"]},\"ez-cam01\":{\"ver\":\"1\"}}";

static ssize_t read_endpoint(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    void *buffer, uint16_t length, uint16_t offset)
{
	enum prov_endpoint endpoint = *(const enum prov_endpoint *)attr->user_data;
	if (endpoint == PROV_VERSION) return bt_gatt_attr_read(conn, attr, buffer, length, offset, version, sizeof(version) - 1);
	k_mutex_lock(&protocol_lock, K_FOREVER);
	ssize_t rc = endpoint != tx_endpoint || tx_length <= 0 ?
		BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION) :
		bt_gatt_attr_read(conn, attr, buffer, length, offset, tx, tx_length);
	k_mutex_unlock(&protocol_lock);
	return rc;
}
static ssize_t write_endpoint(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buffer, uint16_t length, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	if ((size_t)offset + length > sizeof(rx)) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	if (flags & BT_GATT_WRITE_FLAG_PREPARE) return length; /* ATT reassembles on execute. */
	if (offset) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	k_mutex_lock(&protocol_lock, K_FOREVER);
	tx_endpoint = *(const enum prov_endpoint *)attr->user_data;
	tx_length = 0; memcpy(rx, buffer, length);
	int rc = prov_protocol_exchange(&protocol, tx_endpoint, rx, length, tx, sizeof(tx));
	last_protocol_error = rc < 0 ? rc : 0;
	wipe(rx, sizeof(rx));
	if (rc < 0) wipe(tx, sizeof(tx));
	else tx_length = rc;
	k_mutex_unlock(&protocol_lock);
	return rc < 0 ? BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION) : length;
}
#define ENDPOINT(uuid_obj, index, name) \
	BT_GATT_CHARACTERISTIC(&(uuid_obj).uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE, \
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE | BT_GATT_PERM_PREPARE_WRITE, \
		read_endpoint, write_endpoint, (void *)&endpoints[index]), \
	BT_GATT_CUD(name, BT_GATT_PERM_READ)
BT_GATT_SERVICE_DEFINE(provisioning_service,
	BT_GATT_PRIMARY_SERVICE(&service_uuid),
	ENDPOINT(session_uuid, 0, "prov-session"),
	ENDPOINT(config_uuid, 1, "prov-config"),
	ENDPOINT(scan_uuid, 2, "prov-scan"),
	ENDPOINT(version_uuid, 3, "proto-ver"));

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) { (void)k_work_reschedule(&advertise_work, K_MSEC(200)); return; }
	k_spinlock_key_t key = k_spin_lock(&connection_lock);
	connection = bt_conn_ref(conn);
	k_spin_unlock(&connection_lock, key);
	k_mutex_lock(&protocol_lock, K_FOREVER);
	tx_length = 0; prov_protocol_reset(&protocol);
	k_mutex_unlock(&protocol_lock);
}
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn); ARG_UNUSED(reason);
	k_spinlock_key_t key = k_spin_lock(&connection_lock);
	struct bt_conn *old = connection; connection = NULL;
	k_spin_unlock(&connection_lock, key);
	if (old) bt_conn_unref(old);
	k_mutex_lock(&protocol_lock, K_FOREVER);
	prov_protocol_reset(&protocol); wipe(rx, sizeof(rx)); wipe(tx, sizeof(tx)); tx_length = 0;
	k_mutex_unlock(&protocol_lock);
	if (atomic_get(&window_open)) (void)k_work_reschedule(&advertise_work, K_MSEC(200));
}
BT_CONN_CB_DEFINE(provisioning_callbacks) = { .connected = connected, .disconnected = disconnected };
static void advertise(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!atomic_get(&window_open) || !bt_started) return;
	struct bt_conn *c = connection_ref();
	if (c) { bt_conn_unref(c); return; }
	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
		BT_DATA(BT_DATA_UUID128_ALL, service_uuid.val, 16),
	};
	const char *name = bt_get_name();
	const struct bt_data sd[] = { BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name)) };
	last_error = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
}
static void begin_window(struct k_work *work)
{
	ARG_UNUSED(work);
	if (bt_started) return;
	int rc = bt_enable(NULL);
	if (rc) { last_error = rc; return; }
	bt_started = true; atomic_set(&window_open, 1);
	(void)bt_set_name(ez_factory_ble_name());
	advertise(NULL);
	(void)k_work_reschedule(&stop_work, K_SECONDS(CONFIG_EZ_CAM_PROV_WINDOW_SECONDS));
}
static void stop_window(struct k_work *work)
{
	ARG_UNUSED(work); atomic_clear(&window_open);
	(void)k_work_cancel_delayable(&advertise_work);
	if (!bt_started) return;
	(void)bt_le_adv_stop();
	struct bt_conn *c = connection_ref();
	if (c) {
		(void)bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(c);
		(void)k_work_reschedule(&stop_work, K_MSEC(100)); return;
	}
	int rc = bt_disable(); last_error = rc;
	if (!rc) bt_started = false;
}
static void held_reset(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!atomic_get(&button_pressed)) return;
	/* Once cleared, a still-held button must not cause a reboot loop. */
	k_mutex_lock(&config_lock, K_FOREVER);
	bool configured = have_active;
	k_mutex_unlock(&config_lock);
	if (!configured) { (void)k_work_submit(&start_work); return; }
	int rc = settings_delete("ezcam/wifi");
	last_error = rc;
	if (rc && rc != -ENOENT) return;
	sys_reboot(SYS_REBOOT_COLD);
}
void ez_prov_button_changed(bool pressed)
{
	atomic_set(&button_pressed, pressed);
	/* A button held across reboot must be released before another reset. */
	if (pressed && atomic_get(&reset_armed)) {
		(void)k_work_reschedule(&reset_work, K_SECONDS(CONFIG_EZ_CAM_PROV_RESET_HOLD_SECONDS));
	} else if (!pressed) {
		atomic_set(&reset_armed, 1);
		(void)k_work_cancel_delayable(&reset_work);
	}
}
int ez_prov_init(void)
{
	const struct ez_factory_data *factory = ez_factory_get();
	if (!factory) return -ENOENT;
	if (ez_factory_has_wifi(factory)) {
		size_t ssid_len = strlen(factory->ssid), password_len = strlen(factory->password);
		if (password_len < 8 || password_len > 63) return -EINVAL;
		active.magic = CONFIG_MAGIC; active.ssid_len = ssid_len; active.password_len = password_len;
		memcpy(active.ssid, factory->ssid, ssid_len + 1);
		memcpy(active.password, factory->password, password_len + 1);
		have_active = committed = factory_wifi = true;
	}
	int rc = settings_subsys_init();
	if (rc) return rc;
	rc = settings_load_subtree("ezcam");
	if (rc) return rc;
	if (psa_crypto_init() != PSA_SUCCESS || ez_factory_crypto_init()) return -EIO;
	net_mgmt_init_event_callback(&scan_cb, scan_event, NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE);
	net_mgmt_add_event_callback(&scan_cb);
	if (!have_active) (void)k_work_submit(&start_work);
	return 0;
}
#endif

/* SPDX-License-Identifier: Apache-2.0
 * EdgeZ's existing FFF0 / FFF1 RX / FFF2 TX, EZ + uint16-LE framed protobuf.
 * Unprovisioned devices can forward opaque chat packets over HaLow mesh.
 * The companion mesh profile remains separate from station credentials. */
#include "ble_companion.h"
#include "ble_prov.h"
#include "companion_protocol.h"
#include "companion_mesh_platform.h"
#include "factory_data.h"
#include "hardware_config.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ble_companion, LOG_LEVEL_INF);

struct tx_item { uint16_t length; uint8_t frame[COMPANION_PACKET_MAX + 4]; };
/* Do not permanently take camera/newlib heap space for a service that is
 * absent on provisioned boots. Allocate from the kernel heap only while
 * onboarding; release it before starting the provisioned Wi-Fi connection. */
struct companion_buffers {
	uint8_t rx[COMPANION_PACKET_MAX + 4];
	struct tx_item output;
	char queued[3 * sizeof(struct tx_item)] __aligned(4);
};
static struct companion_buffers *buffers;
static struct k_msgq companion_tx;
static K_MUTEX_DEFINE(io_lock);
static struct bt_conn *peer;
static bool registered, notify_enabled, pending;
static size_t received, sent;
static unsigned retries;
static void notify_work(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(tx_work, notify_work);
static struct bt_uuid_16 service_uuid = BT_UUID_INIT_16(0xfff0);
static struct bt_uuid_16 rx_uuid = BT_UUID_INIT_16(0xfff1);
static struct bt_uuid_16 tx_uuid = BT_UUID_INIT_16(0xfff2);

static ssize_t receive(struct bt_conn *conn, const struct bt_gatt_attr *attr,
		       const void *buffer, uint16_t length, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attr);
	if (offset || flags & BT_GATT_WRITE_FLAG_PREPARE) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	k_mutex_lock(&io_lock, K_FOREVER);
	if (!buffers || conn != peer || !notify_enabled || !ez_prov_companion_active(conn)) {
		LOG_WRN("RX rejected: buffers=%u peer=%u subscribed=%u", buffers != NULL, conn == peer, notify_enabled);
		k_mutex_unlock(&io_lock); return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	const uint8_t *data = buffer;
	int rc = 0;
	/* Stream frames so ATT fragments and concatenated frames use the same
	 * bounded parser, independently of negotiated MTU. */
	for (size_t i = 0; i < length; ++i) {
		if (received >= sizeof(buffers->rx)) { rc = -EMSGSIZE; break; }
		buffers->rx[received++] = data[i];
		if (received < 4) continue;
		size_t size = sys_get_le16(buffers->rx + 2);
		if (buffers->rx[0] != 'E' || buffers->rx[1] != 'Z' || !size || size > COMPANION_PACKET_MAX) { rc = -EBADMSG; break; }
		if (received < size + 4) continue;
		struct tx_item item;
		char version[96];
		snprintk(version, sizeof(version), "%s/%s", ez_factory_model(), EZ_DEV54_APP_REV);
		struct companion_identity id = {
			.node = strtoull(ez_factory_device_serial(), NULL, 16),
			.name = ez_prov_device_name(), .version = version,
			.icon = hardware_mode_get() == HARDWARE_CAMERA ? "camera" : "sensor",
			.country = ez_factory_get()->country,
			.gps = hardware_mode_get() == HARDWARE_GPS,
		};
		rc = companion_protocol_exchange(&id, buffers->rx + 4, size, item.frame + 4, COMPANION_PACKET_MAX);
		received = 0; memset(buffers->rx, 0, sizeof(buffers->rx));
		if (rc < 0) break;
		if (rc == 0) continue;
		item.frame[0] = 'E'; item.frame[1] = 'Z'; sys_put_le16(rc, item.frame + 2);
		item.length = rc + 4;
		rc = k_msgq_put(&companion_tx, &item, K_NO_WAIT);
		if (rc) break;
		(void)k_work_reschedule(&tx_work, K_MSEC(1));
	}
	if (rc < 0) { LOG_WRN("Control exchange failed: %d", rc); received = 0; memset(buffers->rx, 0, sizeof(buffers->rx)); }
	k_mutex_unlock(&io_lock);
	return rc < 0 ? BT_GATT_ERR(rc == -EACCES ? BT_ATT_ERR_AUTHORIZATION : BT_ATT_ERR_VALUE_NOT_ALLOWED) : length;
}
static ssize_t subscribe(struct bt_conn *conn, const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	if (value != 0 && value != BT_GATT_CCC_NOTIFY) return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	if (value && !ez_prov_companion_acquire(conn)) {
		LOG_WRN("Companion claim rejected"); return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	LOG_INF("CCC request: %u", value);
	return sizeof(value);
}
static void subscription_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	k_mutex_lock(&io_lock, K_FOREVER);
	notify_enabled = value == BT_GATT_CCC_NOTIFY;
	LOG_INF("Companion notifications: %s", notify_enabled ? "enabled" : "disabled");
	k_mutex_unlock(&io_lock);
	companion_mesh_session(value == BT_GATT_CCC_NOTIFY);
}
static struct bt_gatt_attr attributes[] = {
	BT_GATT_PRIMARY_SERVICE(&service_uuid),
	BT_GATT_CHARACTERISTIC(&rx_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL, receive, NULL),
	BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC_WITH_WRITE_CB(subscription_changed, subscribe, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};
static struct bt_gatt_service service = BT_GATT_SERVICE(attributes);
static void notify_work(struct k_work *work)
{
	ARG_UNUSED(work);
	k_mutex_lock(&io_lock, K_FOREVER);
	if (!buffers || !peer || !notify_enabled || !ez_prov_companion_active(peer)) goto done;
	if (!pending) {
		if (k_msgq_get(&companion_tx, &buffers->output, K_NO_WAIT)) goto done;
		pending = true; sent = 0; retries = 0;
	}
	size_t size = MIN(buffers->output.length - sent, bt_gatt_get_mtu(peer) - 3);
	int rc = bt_gatt_notify(peer, &attributes[4], buffers->output.frame + sent, size);
	if (rc == -ENOMEM || rc == -EAGAIN || rc == -ENOBUFS) {
		if (++retries < 100) { (void)k_work_reschedule(&tx_work, K_MSEC(10)); goto done; }
	}
	if (!rc) { sent += size; retries = 0; }
	else LOG_WRN("Notify failed: %d retries=%u", rc, retries);
	if (rc || sent == buffers->output.length) { pending = false; memset(&buffers->output, 0, sizeof(buffers->output)); }
	if (pending || k_msgq_num_used_get(&companion_tx)) (void)k_work_reschedule(&tx_work, K_MSEC(1));
 done:
	k_mutex_unlock(&io_lock);
}
void ez_companion_deliver(const uint8_t *packet, size_t size)
{
 if (!packet || !size || size > COMPANION_PACKET_MAX) return;
 k_mutex_lock(&io_lock, K_FOREVER);
 if (buffers && peer && notify_enabled && ez_prov_companion_active(peer)) {
  struct tx_item item = {.length = size + 4};
  item.frame[0]='E'; item.frame[1]='Z'; sys_put_le16(size,item.frame+2);
  memcpy(item.frame+4,packet,size);
  if (!k_msgq_put(&companion_tx,&item,K_NO_WAIT)) k_work_reschedule(&tx_work,K_NO_WAIT);
 }
 k_mutex_unlock(&io_lock);
}
void ez_companion_reset(struct bt_conn *conn)
{
	LOG_INF("BLE companion %s", conn ? "connected" : "disconnected");
	/* Finish any in-flight exchange and deny new input before resetting its
	 * mesh profile. Stop outside io_lock so pending RF delivery can finish. */
	k_mutex_lock(&io_lock, K_FOREVER);
	notify_enabled = false;
	k_mutex_unlock(&io_lock);
	companion_mesh_session(false);
	(void)k_work_cancel_delayable(&tx_work);
	k_mutex_lock(&io_lock, K_FOREVER);
	if (peer) bt_conn_unref(peer);
	peer = conn ? bt_conn_ref(conn) : NULL;
	notify_enabled = pending = false; received = sent = retries = 0;
	if (buffers) {
		memset(buffers, 0, sizeof(*buffers));
		k_msgq_purge(&companion_tx);
	}
	k_mutex_unlock(&io_lock);
}
void ez_companion_release_buffers(void)
{
	(void)k_work_cancel_delayable(&tx_work);
	k_mutex_lock(&io_lock, K_FOREVER);
	if (buffers) {
		k_msgq_purge(&companion_tx);
		memset(buffers, 0, sizeof(*buffers));
		k_free(buffers); buffers = NULL;
	}
	notify_enabled = pending = false; received = sent = retries = 0;
	k_mutex_unlock(&io_lock);
}
int ez_companion_register(void)
{
	if (!buffers) {
		buffers = k_calloc(1, sizeof(*buffers));
		if (!buffers) return -ENOMEM;
		k_msgq_init(&companion_tx, buffers->queued, sizeof(struct tx_item), 3);
	}
	if (registered) return 0;
	int rc = bt_gatt_service_register(&service);
	LOG_INF("GATT register: %d RX=%u TX=%u CCC=%u", rc, attributes[2].handle, attributes[4].handle, attributes[5].handle);
	if (!rc) registered = true;
	else ez_companion_release_buffers();
	return rc;
}
void ez_companion_unregister(void)
{
	ez_companion_reset(NULL);
	if (registered && !bt_gatt_service_unregister(&service)) registered = false;
	ez_companion_release_buffers();
}

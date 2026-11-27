/* SPDX-License-Identifier: Apache-2.0 */
#include "cJSON.h"
#include "companion_protocol.h"
#include "companion_mesh_platform.h"
#include "dev01.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "prov_protocol.h"
#include "prov_session.h"
#include "psa/crypto.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#define GRACE_US (180LL * 1000000)
static SemaphoreHandle_t guard;
static bool companion, closing, subscribed, staged_valid;
static uint16_t peer = BLE_HS_CONN_HANDLE_NONE, tx_handle;
static uint8_t own_address_type;
static int64_t deadline, success_deadline;
static dev01_wifi_t staged;
static struct {
  uint8_t rx[512], tx[512], frame[516];
  size_t received;
  int size;
  enum prov_endpoint endpoint;
} *io;
static atomic_uint notification_epoch;
struct mesh_notification {unsigned epoch;size_t size;uint8_t packet[COMPANION_PACKET_MAX];};
static QueueHandle_t mesh_notifications;
static TaskHandle_t mesh_notify_task;
void dev01_companion_deliver(const uint8_t *packet,size_t size) {
 if(!packet || !size || size>COMPANION_PACKET_MAX || !mesh_notifications) return;
 struct mesh_notification item={.epoch=atomic_load(&notification_epoch),.size=size};memcpy(item.packet,packet,size);
 (void)xQueueSend(mesh_notifications,&item,0);
}
static void mesh_notify(void *arg) {
 (void)arg;struct mesh_notification item;
 for(;;) {
  xQueueReceive(mesh_notifications,&item,portMAX_DELAY);
  uint8_t frame[COMPANION_PACKET_MAX+4]={'E','Z',item.size,item.size>>8};
  memcpy(frame+4,item.packet,item.size);
  unsigned retries=0;
  for(size_t offset=0;offset<item.size+4;) {
   xSemaphoreTake(guard,portMAX_DELAY);
   if(item.epoch!=atomic_load(&notification_epoch) || !io || !companion || !subscribed || peer==BLE_HS_CONN_HANDLE_NONE || closing) {xSemaphoreGive(guard);break;}
   size_t take=MIN((size_t)ble_att_mtu(peer)-3,item.size+4-offset);
   struct os_mbuf *buffer=ble_hs_mbuf_from_flat(frame+offset,take);
   int rc=buffer?ble_gatts_notify_custom(peer,tx_handle,buffer):BLE_HS_ENOMEM;
   xSemaphoreGive(guard);
   if(rc) {if(++retries>=100) break;vTaskDelay(pdMS_TO_TICKS(10));continue;}
   retries=0;offset+=take;vTaskDelay(pdMS_TO_TICKS(5));
  }
 }
}
static void wipe(void *data, size_t n) {
  volatile uint8_t *p = data;
  while (n--)
    *p++ = 0;
}
static psa_key_id_t aes_key;
static psa_cipher_operation_t cipher = PSA_CIPHER_OPERATION_INIT;
static void crypto_reset(void *user) {
  (void)user;
  (void)psa_cipher_abort(&cipher);
  if (aes_key)
    (void)psa_destroy_key(aes_key);
  aes_key = 0;
}
static int crypto_begin(void *user, const uint8_t client[32],
                        uint8_t device[32], uint8_t iv[16]) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t key = 0;
  uint8_t secret[32], digest[32];
  size_t length;
  int rc = -EIO;
  (void)user;
  crypto_reset(NULL);
  psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
  psa_set_key_bits(&attr, 255);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
  psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
  if (psa_generate_key(&attr, &key) != PSA_SUCCESS)
    goto done;
  if (psa_export_public_key(key, device, 32, &length) != PSA_SUCCESS ||
      length != 32)
    goto done;
  if (psa_raw_key_agreement(PSA_ALG_ECDH, key, client, 32, secret, 32,
                            &length) != PSA_SUCCESS ||
      length != 32)
    goto done;
  if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)dev01_pop(),
                       strlen(dev01_pop()), digest, sizeof(digest),
                       &length) != PSA_SUCCESS ||
      length != 32)
    goto done;
  for (size_t i = 0; i < 32; ++i)
    secret[i] ^= digest[i];
  if (psa_generate_random(iv, 16) != PSA_SUCCESS)
    goto done;
  psa_reset_key_attributes(&attr);
  psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attr, 256);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
  psa_set_key_algorithm(&attr, PSA_ALG_CTR);
  if (psa_import_key(&attr, secret, 32, &aes_key) != PSA_SUCCESS)
    goto done;
  if (psa_cipher_encrypt_setup(&cipher, aes_key, PSA_ALG_CTR) != PSA_SUCCESS ||
      psa_cipher_set_iv(&cipher, iv, 16) != PSA_SUCCESS)
    goto done;
  rc = 0;
done:
  if (key)
    (void)psa_destroy_key(key);
  psa_reset_key_attributes(&attr);
  wipe(secret, sizeof(secret));
  wipe(digest, sizeof(digest));
  if (rc)
    crypto_reset(NULL);
  return rc;
}
static int crypto_stream(void *user, uint8_t *buffer, size_t size) {
  size_t length = 0;
  uint8_t output[PROV_PACKET_MAX + 16];
  (void)user;
  if (size > PROV_PACKET_MAX)
    return -EMSGSIZE;
  /* Mbed TLS rejects in-place cipher_update for non-block-sized input,
   * including short protobuf messages in CTR mode. Keep buffers distinct. */
  psa_status_t rc =
      psa_cipher_update(&cipher, buffer, size, output, sizeof(output), &length);

  if (rc == PSA_SUCCESS && length == size)
    memcpy(buffer, output, size);
  wipe(output, sizeof(output));
  return rc == PSA_SUCCESS && length == size ? 0 : -EIO;
}

static int set_network(void *user, const uint8_t *ssid, size_t n,
                       const uint8_t *password, size_t p) {
  (void)user;
  if (!n || n > 32 || p < 8 || p > 63 || memchr(ssid, 0, n) ||
      memchr(password, 0, p) || dev01_configured())
    return -EINVAL;
  wipe(&staged, sizeof(staged));
  staged.magic = 0x455a5701U;
  memcpy(staged.ssid, ssid, n);
  memcpy(staged.password, password, p);
  staged_valid = true;
  return 0;
}
static int apply_network(void *user) {
  (void)user;
  if (!staged_valid || companion || closing)
    return -EINVAL;
  int rc = dev01_request_wifi(&staged);
  if (!rc)
    staged_valid = false;
  return rc ? -EIO : 0;
}
static void get_status(void *user, struct prov_status *status) {
  (void)user;
  unsigned value;
  dev01_wifi_status(&value, status->ip);
  status->state = value;
  status->fail_reason = value == 3 ? 1 : 0;
  status->ap.ssid_len = strlen(staged.ssid);
  memcpy(status->ap.ssid, staged.ssid, status->ap.ssid_len);
  status->ap.auth = 6;
  if (!value && dev01_configured() && !success_deadline)
    success_deadline = esp_timer_get_time() + 5000000;
}
static int mqtt_config(void *user, const uint8_t *data, size_t length) {
  (void)user;
  if (memchr(data, 0, length))
    return -EINVAL;
  cJSON *json = cJSON_ParseWithLength((const char *)data, length);
  const cJSON *value =
      json ? cJSON_GetObjectItemCaseSensitive(json, "deviceName") : NULL;
  int rc = cJSON_IsString(value) ? dev01_set_name(value->valuestring)
                                 : ESP_ERR_INVALID_ARG;
  cJSON_Delete(json);
  return rc ? -EINVAL : 0;
}
static const struct prov_ops operations = {.begin = crypto_begin,
                                           .crypt = crypto_stream,
                                           .reset = crypto_reset,
                                           .set = set_network,
                                           .apply = apply_network,
                                           .status = get_status,
                                           .scan = dev01_scan_start,
                                           .scan_status = dev01_scan_status,
                                           .scan_result = dev01_scan_result,
                                           .mqtt_config = mqtt_config};
static struct prov_protocol protocol = {.ops = &operations};
static const char version[] =
    "{\"prov\":{\"ver\":\"v1.1\",\"sec_ver\":1,\"sec_patch_ver\":0,\"cap\":["
    "\"wifi_scan\",\"ez_wifi_prov\",\"mqtt_config\",\"ez_dev01\"]}}";
static int descriptor(uint16_t conn, uint16_t handle,
                      struct ble_gatt_access_ctxt *context, void *arg) {
  (void)conn;
  (void)handle;
  const char *name = arg;
  return os_mbuf_append(context->om, name, strlen(name))
             ? BLE_ATT_ERR_INSUFFICIENT_RES
             : 0;
}
static int provisioning(uint16_t conn, uint16_t handle,
                        struct ble_gatt_access_ctxt *context, void *arg) {
  (void)conn;
  (void)handle;
  enum prov_endpoint endpoint = (uintptr_t)arg;
  xSemaphoreTake(guard, portMAX_DELAY);
  int rc = 0;
  if (!prov_session_provisioning_open(true, closing, companion)) {
    rc = BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    goto done;
  }
  if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    if (endpoint == PROV_VERSION)
      rc = os_mbuf_append(context->om, version, sizeof(version) - 1)
               ? BLE_ATT_ERR_INSUFFICIENT_RES
               : 0;
    else if (io->size > 0 && endpoint == io->endpoint)
      rc = os_mbuf_append(context->om, io->tx, io->size)
               ? BLE_ATT_ERR_INSUFFICIENT_RES
               : 0;
    else
      rc = BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
  } else if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    uint16_t n = OS_MBUF_PKTLEN(context->om), copied = 0;
    if (n > sizeof(io->rx) ||
        ble_hs_mbuf_to_flat(context->om, io->rx, sizeof(io->rx), &copied) ||
        copied != n) {
      rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      goto done;
    }
    io->endpoint = endpoint;
    io->size = prov_protocol_exchange(&protocol, endpoint, io->rx, n, io->tx,
                                      sizeof(io->tx));
    wipe(io->rx, sizeof(io->rx));
    if (io->size < 0) {
      wipe(io->tx, sizeof(io->tx));
      rc = BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }
  } else
    rc = BLE_ATT_ERR_UNLIKELY;
done:
  xSemaphoreGive(guard);
  return rc;
}
static int control(uint16_t conn, uint16_t handle,
                   struct ble_gatt_access_ctxt *context, void *arg) {
  (void)handle;
  (void)arg;
  xSemaphoreTake(guard, portMAX_DELAY);
  int error = 0;
  uint16_t size = OS_MBUF_PKTLEN(context->om), copied;
  if (!companion || closing || !subscribed || conn != peer ||
      dev01_configured()) {
    error = BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    goto done;
  }
  uint8_t chunk[512];
  if (size > sizeof(chunk) ||
      ble_hs_mbuf_to_flat(context->om, chunk, sizeof(chunk), &copied) ||
      copied != size) {
    error = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    goto done;
  }
  for (unsigned i = 0; i < size; i++) {
    if (io->received >= sizeof(io->frame)) {
      error = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      break;
    }
    io->frame[io->received++] = chunk[i];
    if (io->received < 4)
      continue;
    size_t payload = io->frame[2] | ((size_t)io->frame[3] << 8);
    if (io->frame[0] != 'E' || io->frame[1] != 'Z' || !payload ||
        payload > 512) {
      error = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      break;
    }
    if (io->received < payload + 4)
      continue;
    uint8_t response[516];
    uint64_t node = 0;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    for (unsigned j = 0; j < 6; j++)
      node = (node << 8) | mac[j];
    struct companion_identity identity = {
        .node = node,
        .name = dev01_name(),
        .version = esp_app_get_description()->version,
        .icon = "sensor",
        .country = dev01_factory_country(),
        .gps = 0};
    int n = companion_protocol_exchange(&identity, io->frame + 4, payload,
                                        response + 4, 512);
    io->received = 0;
    wipe(io->frame, sizeof(io->frame));
    if (n < 0) {
      error = BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
      break;
    }
    if (n == 0) continue;
    struct mesh_notification item={.epoch=atomic_load(&notification_epoch),.size=(size_t)n};
    memcpy(item.packet,response+4,n);
    if (!xQueueSend(mesh_notifications,&item,0)) {
      error=BLE_ATT_ERR_INSUFFICIENT_RES;
      break;
    }
  }
  if (error) {
    io->received = 0;
    wipe(io->frame, sizeof(io->frame));
  }
done:
  xSemaphoreGive(guard);
  return error;
}
#define U128(a, b)                                                             \
  BLE_UUID128_DECLARE(0x07, 0xed, 0x9b, 0x2d, 0x0f, 0x06, 0x7c, 0x87, 0x9b,    \
                      0x43, 0x43, 0x6b, a, b, 0x75, 0x17)
#define PROV_CHR(a, b, idx, label)                                             \
  {                                                                            \
    .uuid = U128(a, b), .access_cb = provisioning,                             \
    .arg = (void *)(uintptr_t)idx,                                             \
    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,                       \
    .descriptors = (struct ble_gatt_dsc_def[]) {                               \
      {.uuid = BLE_UUID16_DECLARE(0x2901),                                     \
       .att_flags = BLE_ATT_F_READ,                                            \
       .access_cb = descriptor,                                                \
       .arg = (void *)label},                                                  \
      {                                                                        \
        0                                                                      \
      }                                                                        \
    }                                                                          \
  }
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = U128(0x4d, 0x24),
     .characteristics =
         (struct ble_gatt_chr_def[]){PROV_CHR(0x51, 0xff, 0, "prov-session"),
                                     PROV_CHR(0x52, 0xff, 1, "prov-config"),
                                     PROV_CHR(0x50, 0xff, 2, "prov-scan"),
                                     PROV_CHR(0x53, 0xff, 3, "proto-ver"),
                                     PROV_CHR(0x54, 0xff, 4, "mqtt-config"),
                                     {0}}},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID16_DECLARE(0xfff0),
     .characteristics =
         (struct ble_gatt_chr_def[]){{.uuid = BLE_UUID16_DECLARE(0xfff1),
                                      .access_cb = control,
                                      .flags = BLE_GATT_CHR_F_WRITE},
                                     {.uuid = BLE_UUID16_DECLARE(0xfff2),
                                      .val_handle = &tx_handle,
                                      .access_cb = control,
                                      .flags = BLE_GATT_CHR_F_NOTIFY},
                                     {0}}},
    {0}};
static int advertise(void);
static int gap(struct ble_gap_event *event, void *arg) {
  (void)arg;
  /* NimBLE dispatches NOTIFY_TX synchronously inside notify_custom(). RX
   * already owns guard then; unhandled events must not acquire it again. */
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
  case BLE_GAP_EVENT_SUBSCRIBE:
  case BLE_GAP_EVENT_DISCONNECT:
  case BLE_GAP_EVENT_ADV_COMPLETE:
    break;
  default:
    return 0;
  }
  xSemaphoreTake(guard, portMAX_DELAY);
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (!event->connect.status) {
      peer = event->connect.conn_handle;
      if (closing)
        ble_gap_terminate(peer, BLE_ERR_REM_USER_CONN_TERM);
    } else if (!closing)
      advertise();
    break;
  case BLE_GAP_EVENT_SUBSCRIBE:
    if (event->subscribe.attr_handle == tx_handle) {
      subscribed = event->subscribe.cur_notify;
      if (!subscribed) companion_mesh_session(false);
      if (subscribed &&
          prov_session_can_claim(true, !dev01_companion_available(),
                                 event->subscribe.conn_handle == peer, true,
                                 closing)) {
        ESP_LOGI("ez_dev01_ble", "Companion claimed; provisioning closed");
        companion = true;
        companion_mesh_session(true);
        staged_valid = false;
        wipe(&staged, sizeof(staged));
        prov_protocol_reset(&protocol);
        wipe(io, sizeof(*io));
      }
    }
    break;
  case BLE_GAP_EVENT_DISCONNECT:
    ESP_LOGI("ez_dev01_ble",
             "BLE disconnected; restoring provisioning for 180 seconds");
    peer = BLE_HS_CONN_HANDLE_NONE;
    companion_mesh_session(false);
    atomic_fetch_add(&notification_epoch,1);
    xQueueReset(mesh_notifications);
    companion = subscribed = staged_valid = false;
    wipe(&staged, sizeof(staged));
    deadline = esp_timer_get_time() + GRACE_US;
    prov_protocol_reset(&protocol);
    wipe(io, sizeof(*io));
    if (!closing)
      advertise();
    break;
  case BLE_GAP_EVENT_ADV_COMPLETE:
    if (!closing)
      advertise();
    break;
  default:
    break;
  }
  xSemaphoreGive(guard);
  return 0;
}
static int advertise(void) {
  static const ble_uuid128_t prov_uuid =
      BLE_UUID128_INIT(0x07, 0xed, 0x9b, 0x2d, 0x0f, 0x06, 0x7c, 0x87, 0x9b,
                       0x43, 0x43, 0x6b, 0x4d, 0x24, 0x75, 0x17);
  static const ble_uuid16_t control_uuid = BLE_UUID16_INIT(0xfff0);
  struct ble_hs_adv_fields fields = {.flags = BLE_HS_ADV_F_DISC_GEN |
                                              BLE_HS_ADV_F_BREDR_UNSUP,
                                     .uuids128 = &prov_uuid,
                                     .num_uuids128 = 1,
                                     .uuids128_is_complete = 1,
                                     .uuids16 = &control_uuid,
                                     .num_uuids16 = 1,
                                     .uuids16_is_complete = 1};
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc)
    return rc;
  const char *name = ble_svc_gap_device_name();
  struct ble_hs_adv_fields scan = {.name = (const uint8_t *)name,
                                   .name_len = strlen(name),
                                   .name_is_complete = 1};
  if ((rc = ble_gap_adv_rsp_set_fields(&scan)))
    return rc;
  struct ble_gap_adv_params parameters = {.conn_mode = BLE_GAP_CONN_MODE_UND,
                                          .disc_mode = BLE_GAP_DISC_MODE_GEN};
  return ble_gap_adv_start(own_address_type, NULL, BLE_HS_FOREVER, &parameters,
                           gap, NULL);
}
static void sync_host(void) {
  if (!ble_hs_id_infer_auto(0, &own_address_type))
    advertise();
}
static void host_task(void *arg) {
  (void)arg;
  nimble_port_run();
  nimble_port_freertos_deinit();
}
esp_err_t dev01_onboarding(void) {
  io = calloc(1, sizeof(*io));
  guard = xSemaphoreCreateMutex();
  if (!io || !guard)
    return ESP_ERR_NO_MEM;
  mesh_notifications = xQueueCreate(3, sizeof(struct mesh_notification));
  if (!mesh_notifications || xTaskCreate(mesh_notify, "mesh_ble_tx", 4096, NULL, 4, &mesh_notify_task) != pdPASS) return ESP_ERR_NO_MEM;
  if (nimble_port_init() != ESP_OK)
    return ESP_FAIL;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_hs_cfg.sync_cb = sync_host;
  char name[32];
  if (strlen(dev01_identity.serial) > 26)
    return ESP_ERR_INVALID_SIZE;
  snprintf(name, sizeof(name), "EZ_%s", dev01_identity.serial);
  ble_svc_gap_device_name_set(name);
  if (ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services))
    return ESP_FAIL;
  ESP_LOGI("ez_dev01_ble",
           "Onboarding %s; companion available until provisioned", name);
  deadline = esp_timer_get_time() + GRACE_US;
  nimble_port_freertos_init(host_task);
  for (;;) {
    xSemaphoreTake(guard, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    bool configured = dev01_configured();
    if (configured && !success_deadline)
      success_deadline = now + 5000000;
    bool finish = (configured && now >= success_deadline) ||
                  (!configured &&
                   !prov_session_hold_awake(peer != BLE_HS_CONN_HANDLE_NONE,
                                            configured, closing) &&
                   now >= deadline);
    uint16_t closing_peer = peer;
    if (finish)
      closing = true;
    xSemaphoreGive(guard);
    if (finish) {
      ble_gap_adv_stop();
      if (closing_peer != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(closing_peer, BLE_ERR_REM_USER_CONN_TERM);
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  for (unsigned i = 0; i < 100 && peer != BLE_HS_CONN_HANDLE_NONE; i++)
    vTaskDelay(pdMS_TO_TICKS(50));
  companion_mesh_session(false);
  vTaskDelete(mesh_notify_task);
  vQueueDelete(mesh_notifications); mesh_notifications = NULL;
  nimble_port_stop();
  nimble_port_deinit();
  prov_protocol_reset(&protocol);
  wipe(io, sizeof(*io));
  free(io);
  io = NULL;
  vSemaphoreDelete(guard);
  return dev01_configured() ? ESP_OK : ESP_ERR_TIMEOUT;
}

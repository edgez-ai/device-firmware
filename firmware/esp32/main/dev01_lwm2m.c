/* SPDX-License-Identifier: Apache-2.0 */
#include "dev01.h"
#include "dev01_diag.h"
#include "dev01_network.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "liblwm2m.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "lwm2mclient.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
struct session {
  int socket;
  struct sockaddr_in server;
};
void *lwm2m_malloc(size_t n) { return malloc(n); }
void lwm2m_free(void *p) { free(p); }
char *lwm2m_strdup(const char *s) { return s ? strdup(s) : NULL; }
int lwm2m_strncmp(const char *a, const char *b, size_t n) {
  return strncmp(a, b, n);
}
int lwm2m_strcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }
time_t lwm2m_gettime(void) { return esp_timer_get_time() / 1000000; }
int lwm2m_seed(void) { return esp_random(); }
void lwm2m_printf(const char *format, ...) { (void)format; }
void *lwm2m_connect_server(uint16_t id, void *data) {
  return id == 0 ? data : NULL;
}
void lwm2m_close_connection(void *session, void *data) {
  (void)session;
  (void)data;
}
void lwm2m_session_remove(void *session) { (void)session; }
bool lwm2m_session_is_equal(void *a, void *b, void *data) {
  (void)data;
  return a == b;
}
uint8_t lwm2m_buffer_send(void *handle, uint8_t *buffer, size_t n, void *data) {
  (void)data;
  struct session *session = handle;
  return sendto(session->socket, buffer, n, 0,
                (struct sockaddr *)&session->server,
                sizeof(session->server)) == (ssize_t)n
             ? COAP_NO_ERROR
             : COAP_503_SERVICE_UNAVAILABLE;
}
static uint8_t read_device(lwm2m_context_t *ctx, uint16_t instance, int *count,
                           lwm2m_data_t **data, lwm2m_object_t *object) {
  (void)ctx;
  if (instance)
    return COAP_404_NOT_FOUND;
  const uint16_t device[] = {0, 1, 2, 3, 11, 16}, hardware[] = {0, 1, 7};
  const uint16_t *ids = object->objID == 3 ? device : hardware;
  size_t size = object->objID == 3 ? 6 : 3;
  if (!*count) {
    *data = lwm2m_data_new(size);
    if (!*data)
      return COAP_500_INTERNAL_SERVER_ERROR;
    *count = size;
    for (int i = 0; i < *count; i++)
      (*data)[i].id = ids[i];
  }
  for (int i = 0; i < *count; i++) {
    lwm2m_data_t *value = *data + i;
    if (object->objID == 31029) {
      if (value->id == 7)
        lwm2m_data_encode_string(
            *dev01_name() ? dev01_name() : dev01_identity.serial, value);
      else if (value->id == 0 || value->id == 1)
        lwm2m_data_encode_int(0, value);
      else
        return COAP_404_NOT_FOUND;
      continue;
    }
    switch (value->id) {
    case 0:
      lwm2m_data_encode_string("EdgeZ", value);
      break;
    case 1:
      lwm2m_data_encode_string(dev01_model(), value);
      break;
    case 2:
      lwm2m_data_encode_string(dev01_identity.serial, value);
      break;
    case 3:
      lwm2m_data_encode_string(esp_app_get_description()->version, value);
      break;
    case 11: {
      lwm2m_data_t *child = lwm2m_data_new(1);
      if (!child)
        return COAP_500_INTERNAL_SERVER_ERROR;
      child->id = 0;
      lwm2m_data_encode_int(0, child);
      lwm2m_data_encode_instances(child, 1, value);
      break;
    }
    case 16:
      lwm2m_data_encode_string("U", value);
      break;
    default:
      return COAP_404_NOT_FOUND;
    }
  }
  return COAP_205_CONTENT;
}
static uint8_t execute_device(lwm2m_context_t *ctx, uint16_t instance,
                              uint16_t resource, uint8_t *args, int size,
                              lwm2m_object_t *object) {
  (void)ctx;
  (void)args;
  (void)object;
  if (instance)
    return COAP_404_NOT_FOUND;
  if (size)
    return COAP_400_BAD_REQUEST;
  if (resource != 4 && resource != 5)
    return COAP_405_METHOD_NOT_ALLOWED;
  dev01_reboot(resource == 5);
  return COAP_204_CHANGED;
}
struct dhcp_lookup {
  uint8_t index;
  uint32_t server;
};
static esp_err_t read_dhcp_server(void *arg) {
  struct dhcp_lookup *lookup = arg;
  struct netif *netif = netif_get_by_index(lookup->index);
  struct dhcp *dhcp = netif ? netif_dhcp_data(netif) : NULL;
  if (dhcp && IP_IS_V4(&dhcp->server_ip_addr))
    lookup->server = ip4_addr_get_u32(ip_2_ip4(&dhcp->server_ip_addr));
  return ESP_OK;
}
static uint32_t current_server(esp_netif_t *netif) {
  esp_netif_ip_info_t ip;
  if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || !ip.ip.addr)
    return 0;
  struct dhcp_lookup lookup = {0};
  if (!ip.gw.addr) {
    int index = esp_netif_get_netif_impl_index(netif);
    if (index > 0 && index <= UINT8_MAX) {
      lookup.index = index;
      if (esp_netif_tcpip_exec(read_dhcp_server, &lookup) != ESP_OK)
        return 0;
    }
  }
  return dev01_lwm2m_server(ip.ip.addr, ip.netmask.addr, ip.gw.addr,
                            lookup.server);
}
static void client_task(void *arg) {
  (void)arg;
  for (;;) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_ip4_addr_t server = {.addr = current_server(netif)};
    if (!server.addr) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    struct session session = {.socket =
                                  socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP),
                              .server = {.sin_family = AF_INET,
                                         .sin_port = htons(5683),
                                         .sin_addr.s_addr = server.addr}};
    if (session.socket < 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    char uri[64];
    snprintf(uri, sizeof(uri), "coap://" IPSTR ":5683", IP2STR(&server));
    ESP_LOGI("ez_dev01_lwm2m", "Registering endpoint %s at %s",
             dev01_identity.serial, uri);
    lwm2m_list_t device_instance = {.id = 0}, hardware_instance = {.id = 0};
    lwm2m_object_t device = {.objID = 3,
                             .instanceList = &device_instance,
                             .readFunc = read_device,
                             .executeFunc = execute_device};
    lwm2m_object_t hardware = {.objID = 31029,
                               .instanceList = &hardware_instance,
                               .readFunc = read_device};
    lwm2m_object_t *objects[] = {
        get_security_object(1, uri, NULL, NULL, 0, false),
        get_server_object(1, "U", 300, false), &device, &hardware,
        dev01_diag_object()};
    lwm2m_context_t *context = lwm2m_init(&session);
    bool ready =
        context && objects[0] && objects[1] &&
        lwm2m_configure(context, dev01_identity.serial, NULL, NULL,
                        sizeof(objects) / sizeof(objects[0]), objects) == 0;
    while (ready) {
      if (current_server(netif) != session.server.sin_addr.s_addr)
        break;
      time_t timeout = 1;
      if (lwm2m_step(context, &timeout))
        break;
      fd_set read;
      FD_ZERO(&read);
      FD_SET(session.socket, &read);
      struct timeval wait = {.tv_sec = timeout < 1 ? 0 : 1,
                             .tv_usec = timeout < 1 ? 10000 : 0};
      int result = select(session.socket + 1, &read, NULL, NULL, &wait);
      if (result < 0 && errno != EINTR)
        break;
      if (result > 0) {
        uint8_t buffer[1536];
        struct sockaddr_in from;
        socklen_t length = sizeof(from);
        ssize_t n = recvfrom(session.socket, buffer, sizeof(buffer), 0,
                             (struct sockaddr *)&from, &length);
        if (n > 0 && from.sin_addr.s_addr == session.server.sin_addr.s_addr &&
            from.sin_port == session.server.sin_port)
          lwm2m_handle_packet(context, buffer, n, &session);
      }
    }
    if (context)
      lwm2m_close(context);
    if (objects[0]) {
      clean_security_object(objects[0]);
      lwm2m_free(objects[0]);
    }
    if (objects[1]) {
      clean_server_object(objects[1]);
      lwm2m_free(objects[1]);
    }
    close(session.socket);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
void dev01_lwm2m_start(void) {
  dev01_diag_start();
  if (xTaskCreate(client_task, "lwm2m", 8192, NULL, 4, NULL) != pdPASS)
    abort();
}

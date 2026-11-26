/* SPDX-License-Identifier: Apache-2.0 */
#include "dev01_board.h"
#include "dev01_led.h"
#include "dev01_observe.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_err.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static rmt_channel_handle_t channel;
static rmt_encoder_handle_t encoder;
static bool light_on;
static SemaphoreHandle_t led_lock;
/* Caller owns led_lock, including when deriving a toggle from light_on. */
static esp_err_t transmit_locked(bool on) {
  /* SK6812: GRB plus zero white byte (also accepts RGBW variants).
   * 10 MHz ticks, 1.2 us/bit, reset low for 100 us. */
  rmt_symbol_word_t symbols[33] = {0};
  for (unsigned i = 0; i < 32; ++i) {
    bool bit = on && i < 24 && ((8U >> (7 - i % 8)) & 1U);
    symbols[i] = (rmt_symbol_word_t){.level0=1, .duration0=bit ? 6 : 3,
                                   .level1=0, .duration1=bit ? 6 : 9};
  }
  symbols[32] = (rmt_symbol_word_t){.level0=0,.duration0=500,.level1=0,.duration1=500};
  rmt_transmit_config_t config = {.loop_count=0};
  esp_err_t rc = rmt_transmit(channel, encoder, symbols, sizeof(symbols), &config);
  if (rc == ESP_OK) rc = rmt_tx_wait_all_done(channel, -1);
  if (rc == ESP_OK && light_on != on) {
    light_on = on;
    dev01_observe_changed(DEV01_CHANGED_LED);
  }
  return rc;
}
static esp_err_t set_light(bool on) {
  xSemaphoreTake(led_lock, portMAX_DELAY);
  esp_err_t rc = transmit_locked(on);
  xSemaphoreGive(led_lock);
  return rc;
}
void dev01_led_off(void) { ESP_ERROR_CHECK(set_light(false)); }
void dev01_led_toggle(void) {
  xSemaphoreTake(led_lock, portMAX_DELAY);
  esp_err_t rc = transmit_locked(!light_on);
  xSemaphoreGive(led_lock);
  ESP_ERROR_CHECK(rc);
}
void dev01_led_init(void) {
  led_lock = xSemaphoreCreateMutex();
  if (!led_lock) abort();
  rmt_tx_channel_config_t config = {.gpio_num=DEV01_LED_GPIO,
      .clk_src=RMT_CLK_SRC_DEFAULT, .resolution_hz=10000000,
      .mem_block_symbols=64, .trans_queue_depth=1};
  rmt_copy_encoder_config_t copy = {};
  ESP_ERROR_CHECK(rmt_new_tx_channel(&config, &channel));
  ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy, &encoder));
  ESP_ERROR_CHECK(rmt_enable(channel));
  dev01_led_off();
}
static uint8_t discover(lwm2m_context_t *ctx, uint16_t id, int *count,
                        lwm2m_data_t **data, lwm2m_object_t *obj) {
  (void)ctx; (void)obj;
  if (id) return COAP_404_NOT_FOUND;
  if (!*count) {
    *data = lwm2m_data_new(1);
    if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
    *count=1; (*data)[0].id=5850;
  }
  for (int i=0;i<*count;++i) if ((*data)[i].id!=5850) return COAP_404_NOT_FOUND;
  return COAP_205_CONTENT;
}
static uint8_t read_light(lwm2m_context_t *ctx, uint16_t id, int *count,
                          lwm2m_data_t **data, lwm2m_object_t *obj) {
  uint8_t rc=discover(ctx,id,count,data,obj);
  if (rc!=COAP_205_CONTENT) return rc;
  xSemaphoreTake(led_lock, portMAX_DELAY);
  bool on = light_on;
  xSemaphoreGive(led_lock);
  for (int i=0;i<*count;++i) lwm2m_data_encode_bool(on,*data+i);
  return COAP_205_CONTENT;
}
static uint8_t write_light(lwm2m_context_t *ctx, uint16_t id, int count,
                           lwm2m_data_t *data, lwm2m_object_t *obj,
                           lwm2m_write_type_t type) {
  (void)ctx; (void)obj; (void)type;
  if (id) return COAP_404_NOT_FOUND;
  if (count!=1) return COAP_400_BAD_REQUEST;
  if (data[0].id!=5850) return COAP_404_NOT_FOUND;
  bool on;
  if (!lwm2m_data_decode_bool(data,&on)) return COAP_400_BAD_REQUEST;
  if (set_light(on)!=ESP_OK) return COAP_500_INTERNAL_SERVER_ERROR;
  return COAP_204_CHANGED;
}
static lwm2m_list_t instance={.id=0};
static lwm2m_object_t object={.objID=3311,.instanceList=&instance,
    .discoverFunc=discover,.readFunc=read_light,.writeFunc=write_light};
lwm2m_object_t *dev01_led_object(void) { return &object; }

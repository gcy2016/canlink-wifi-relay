// CANLink Wi-Fi relay - ESP32-S3 firmware entry point.
//
// USB Host (native USB port) <-> slcan line relay <-> WebSocket over softAP.
// This is the first bring-up revision: it only forwards recorded traffic and
// slcan commands. It does not touch the CANLink web app and has no flashing
// functionality.

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "slcan_relay.h"
#include "usb_cdc.h"
#include "wifi_ap.h"
#include "ws_server.h"

static const char *TAG = "main";

#if CONFIG_RELAY_STATUS_LED_GPIO >= 0
static void status_led_init(void) {
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << CONFIG_RELAY_STATUS_LED_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
  gpio_set_level(CONFIG_RELAY_STATUS_LED_GPIO, 0);
}
#endif

static void status_task(void *arg) {
  (void)arg;
  uint32_t prev_frames = 0;
  uint32_t prev_ms = 0;
  bool led_on = false;
  TickType_t last_ping = xTaskGetTickCount();

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));

    relay_status_t st;
    relay_get_status(&st);
    char ip[16] = "0.0.0.0";
    wifi_get_ap_ip(ip, sizeof(ip));

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t dt_ms = now_ms - prev_ms;
    const uint32_t fps =
        dt_ms > 0 ? (st.frames_rx - prev_frames) * 1000 / dt_ms : 0;
    prev_frames = st.frames_rx;
    prev_ms = now_ms;

    ESP_LOGI(TAG,
             "usb=%s ap=%s sta=%s ip=%s ws=%u fps=%lu frames=%lu tx=%lu "
             "drops(usb=%lu line=%lu tx=%lu ws=%lu) heap=%lu",
             st.usb_connected ? "up" : "down", wifi_ap_up() ? "up" : "down",
             wifi_sta_up() ? "up" : "down", ip, (unsigned)st.ws_clients,
             (unsigned long)fps, (unsigned long)st.frames_rx,
             (unsigned long)st.lines_tx, (unsigned long)st.usb_overflow,
             (unsigned long)st.line_dropped, (unsigned long)st.tx_dropped,
             (unsigned long)st.ws_dropped,
             (unsigned long)esp_get_free_heap_size());

#if CONFIG_RELAY_WS_PING_INTERVAL_S > 0
    if ((xTaskGetTickCount() - last_ping) >=
        pdMS_TO_TICKS(CONFIG_RELAY_WS_PING_INTERVAL_S * 1000)) {
      last_ping = xTaskGetTickCount();
      ws_server_ping_all();
    }
#endif

#if CONFIG_RELAY_STATUS_LED_GPIO >= 0
    // Solid while the adapter is open, 1 Hz blink otherwise.
    led_on = st.usb_connected ? true : !led_on;
    gpio_set_level(CONFIG_RELAY_STATUS_LED_GPIO, led_on ? 1 : 0);
#endif
  }
}

static esp_err_t nvs_init(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "erasing NVS: %s", esp_err_to_name(err));
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
    err = nvs_flash_init();
  }
  return err;
}

void app_main(void) {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  ESP_LOGI(TAG, "CANLink Wi-Fi relay %s, ESP-IDF %s, %d core(s)",
           CONFIG_APP_PROJECT_VER, esp_get_idf_version(), chip.cores);
  ESP_LOGI(TAG, "reset reason: %d", (int)esp_reset_reason());

  ESP_ERROR_CHECK(nvs_init());
#if CONFIG_RELAY_STATUS_LED_GPIO >= 0
  status_led_init();
#endif

  // Netif/lwIP must exist before the HTTP server opens its listening socket.
  ESP_ERROR_CHECK(wifi_start());

  // relay_init() first: the WebSocket handler starts accepting commands as
  // soon as ws_server_start() returns.
  ESP_ERROR_CHECK(relay_init());
  ESP_ERROR_CHECK(ws_server_start());
  ESP_ERROR_CHECK(usb_cdc_start(relay_on_usb_rx, relay_on_usb_state));
  ESP_ERROR_CHECK(relay_start());

  char ip[16] = "192.168.4.1";
  wifi_get_ap_ip(ip, sizeof(ip));
  ESP_LOGI(TAG, "join Wi-Fi \"%s\", then open http://%s/ or ws://%s/ws",
           CONFIG_RELAY_AP_SSID, ip, ip);

  if (xTaskCreate(status_task, "status", 4096, NULL, 3, NULL) != pdPASS) {
    ESP_LOGE(TAG, "status task not created");
  }
}

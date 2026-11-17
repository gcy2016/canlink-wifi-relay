// Wi-Fi for the relay box.
//
// AP mode is the primary link: the phone joins "CANLink-Relay" and talks to
// 192.168.4.1. STA mode is reserved for the next round (uplink to an existing
// network) and is only enabled when credentials exist in NVS.
//
// "Reconnect" in AP mode means the phone re-joins by itself; a STADISCONNECTED
// event is only logged. If the AP itself stops (driver recovery), the
// supervisor task restarts Wi-Fi. In STA mode a disconnect triggers a connect
// retry with exponential backoff.

#include "wifi_ap.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

#define STA_NVS_NS "relay_cfg"
#define STA_NVS_SSID "sta_ssid"
#define STA_NVS_PASS "sta_pass"

static volatile bool s_ap_up;
static volatile bool s_sta_up;
static volatile bool s_sta_configured;
static TaskHandle_t s_supervisor;
static char s_sta_ssid[33];
static char s_sta_pass[65];

static void load_sta_credentials(void) {
  nvs_handle_t nvs;
  if (nvs_open(STA_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
    return;
  }
  size_t len = sizeof(s_sta_ssid);
  if (nvs_get_str(nvs, STA_NVS_SSID, s_sta_ssid, &len) == ESP_OK && s_sta_ssid[0]) {
    len = sizeof(s_sta_pass);
    if (nvs_get_str(nvs, STA_NVS_PASS, s_sta_pass, &len) != ESP_OK) {
      s_sta_pass[0] = '\0';
    }
    s_sta_configured = true;
  }
  nvs_close(nvs);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data) {
  (void)arg;
  if (base == WIFI_EVENT) {
    switch (id) {
      case WIFI_EVENT_AP_START:
        s_ap_up = true;
        ESP_LOGI(TAG, "softAP started, ssid=\"%s\"", CONFIG_RELAY_AP_SSID);
        break;
      case WIFI_EVENT_AP_STOP:
        s_ap_up = false;
        ESP_LOGW(TAG, "softAP stopped");
        if (s_supervisor) {
          xTaskNotifyGive(s_supervisor);
        }
        break;
      case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "phone joined, aid=%d", e->aid);
        break;
      }
      case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "station left, aid=%d", e->aid);
        break;
      }
      case WIFI_EVENT_STA_START:
        if (s_sta_configured) {
          esp_wifi_connect();
        }
        break;
      case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        s_sta_up = false;
        ESP_LOGW(TAG, "STA disconnected, reason=%d", e->reason);
        if (s_supervisor) {
          xTaskNotifyGive(s_supervisor);
        }
        break;
      }
      default:
        break;
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    s_sta_up = true;
    ESP_LOGI(TAG, "STA got ip " IPSTR, IP2STR(&e->ip_info.ip));
  }
}

static void wifi_supervisor_task(void *arg) {
  (void)arg;
  uint32_t backoff_ms = 2000;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));

    if (!s_ap_up) {
      // AP_STOP is rare (driver error / regulatory change). Bring it back.
      ESP_LOGW(TAG, "restarting Wi-Fi to recover the softAP");
      esp_wifi_stop();
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_err_t err = esp_wifi_start();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
      }
      continue;
    }

    if (s_sta_configured && !s_sta_up) {
      vTaskDelay(pdMS_TO_TICKS(backoff_ms));
      if (backoff_ms < 60000) {
        backoff_ms *= 2;
      }
      ESP_LOGI(TAG, "STA reconnect attempt");
      esp_err_t err = esp_wifi_connect();
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
      }
    } else if (s_sta_up) {
      backoff_ms = 2000;
    }
  }
}

static void build_ap_config(wifi_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  snprintf((char *)cfg->ap.ssid, sizeof(cfg->ap.ssid), "%s", CONFIG_RELAY_AP_SSID);
  cfg->ap.ssid_len = 0;
  cfg->ap.channel = CONFIG_RELAY_AP_CHANNEL;
  cfg->ap.max_connection = CONFIG_RELAY_AP_MAX_STA;
  cfg->ap.beacon_interval = 100;
  cfg->ap.pmf_cfg.required = false;

  const size_t pass_len = strlen(CONFIG_RELAY_AP_PASSWORD);
  if (pass_len >= 8 && pass_len <= 63) {
    snprintf((char *)cfg->ap.password, sizeof(cfg->ap.password), "%s",
             CONFIG_RELAY_AP_PASSWORD);
    cfg->ap.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    cfg->ap.authmode = WIFI_AUTH_OPEN;
    ESP_LOGW(TAG, "AP password is not 8..63 chars, starting an OPEN network");
  }
}

esp_err_t wifi_start(void) {
  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }

  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                          WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL),
                      TAG, "wifi handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                          IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL),
                      TAG, "ip handler");

  load_sta_credentials();

  esp_netif_create_default_wifi_ap();
  wifi_config_t ap_cfg;
  build_ap_config(&ap_cfg);

  wifi_mode_t mode = WIFI_MODE_AP;
  if (s_sta_configured) {
    esp_netif_create_default_wifi_sta();
    mode = WIFI_MODE_APSTA;
    ESP_LOGI(TAG, "STA uplink configured for \"%s\"", s_sta_ssid);
  }

  // Credentials live in our own NVS namespace only; do not let the driver keep
  // a second copy.
  ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), TAG, "wifi mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "ap config");

  if (s_sta_configured) {
    wifi_config_t sta_cfg;
    memset(&sta_cfg, 0, sizeof(sta_cfg));
    strlcpy((char *)sta_cfg.sta.ssid, s_sta_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, s_sta_pass,
            sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG,
                        "sta config");
  }
  ESP_RETURN_ON_ERROR(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20), TAG,
                      "bandwidth");
  // Lower latency for the WebSocket stream; also disables modem sleep for STA.
  esp_wifi_set_ps(WIFI_PS_NONE);
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

  if (xTaskCreatePinnedToCore(wifi_supervisor_task, "wifi_sup", 4096, NULL, 4,
                              &s_supervisor, 0) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

bool wifi_ap_up(void) { return s_ap_up; }
bool wifi_sta_up(void) { return s_sta_up; }

esp_err_t wifi_get_ap_ip(char *buf, size_t len) {
  if (buf == NULL || len < 8) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (netif == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
    return ESP_ERR_INVALID_STATE;
  }
  snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
  return ESP_OK;
}

esp_err_t wifi_set_sta_credentials(const char *ssid, const char *password) {
  if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) {
    return ESP_ERR_INVALID_ARG;
  }
  if (password != NULL && strlen(password) > 64) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(STA_NVS_NS, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_str(nvs, STA_NVS_SSID, ssid);
  if (err == ESP_OK) {
    err = nvs_set_str(nvs, STA_NVS_PASS, password != NULL ? password : "");
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "STA credentials stored, reboot to apply");
  }
  return err;
}

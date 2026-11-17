// Wi-Fi side: always-on softAP for the phone browser, plus a reserved STA
// uplink that is only started when credentials were stored in NVS.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_start(void);

bool wifi_ap_up(void);
bool wifi_sta_up(void);

// "192.168.4.1" by default. Returns ESP_ERR_INVALID_STATE before the AP is up.
esp_err_t wifi_get_ap_ip(char *buf, size_t len);

// Reserved STA configuration port. Stores credentials in NVS; they are read at
// boot, so a reboot is needed before the uplink is actually used.
esp_err_t wifi_set_sta_credentials(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

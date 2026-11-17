idf_component_register(
    SRCS "main.c" "wifi_ap.c" "usb_cdc.c" "ws_server.c" "slcan_relay.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash esp_http_server usb driver esp_timer
)

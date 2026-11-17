// USB Host side: enumerate a slcan adapter (CDC-ACM) on the ESP32-S3 native
// USB port and expose it as a byte pipe.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called from the CDC-ACM driver task with raw bytes received from the adapter.
// Must not block: the relay hands them to a stream buffer and returns.
typedef void (*usb_cdc_rx_cb_t)(const uint8_t *data, size_t len);

// Called from the USB link task whenever the adapter is opened or lost.
typedef void (*usb_cdc_state_cb_t)(bool connected);

// Installs the USB host + CDC-ACM class driver and starts the link task.
// The link task keeps looking for the adapter and reopens it after a
// disconnect, so this is called once at boot.
esp_err_t usb_cdc_start(usb_cdc_rx_cb_t rx_cb, usb_cdc_state_cb_t state_cb);

bool usb_cdc_connected(void);

// Blocking write to the adapter. Returns ESP_ERR_INVALID_STATE when no adapter
// is open. Safe to call from a single writer task (the relay TX task).
esp_err_t usb_cdc_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

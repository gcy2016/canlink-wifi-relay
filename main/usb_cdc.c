// USB Host CDC-ACM link for a slcan adapter (CANable 2.0 Pro / MKS V2.0 with
// slcan or vdlink firmware).
//
// Wiring: the adapter must be plugged into the ESP32-S3 *native* USB port
// (GPIO19 = D-, GPIO20 = D+). The UART Type-C port is only used for power and
// flashing; USB-Serial-JTAG and USB Host share the same PHY, so the native port
// cannot be used as a serial console while it is hosting.
//
// Notes / pitfalls encoded below:
//  * cdc_acm_host_open() must be called again after a disconnect; the driver
//    does not reopen the device by itself.
//  * Some slcan firmwares only start streaming after DTR/RTS are asserted, so
//    the control line state is set explicitly after open.
//  * Closing the device from inside the event callback deadlocks the class
//    driver, so the callback only raises a flag and the link task closes it.

#include "usb_cdc.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

static const char *TAG = "usb";

typedef struct {
  uint16_t vid;
  uint16_t pid;
  const char *name;
} usb_candidate_t;

// Preferred pair first, then the IDs commonly seen on slcan adapters.
static const usb_candidate_t k_candidates[] = {
    {CONFIG_RELAY_USB_VID, CONFIG_RELAY_USB_PID, "configured adapter"},
#if CONFIG_RELAY_USB_TRY_KNOWN_DEVICES
    {0x1D50, 0x606F, "CANable 1.0 / MKS V2.0"},
    {0x0483, 0x5740, "STM32 virtual COM port"},
#endif
};

static cdc_acm_dev_hdl_t s_handle;
static SemaphoreHandle_t s_lock;
static volatile bool s_dev_lost;
static volatile bool s_connected;
static usb_cdc_rx_cb_t s_rx_cb;
static usb_cdc_state_cb_t s_state_cb;

static bool on_data(const uint8_t *data, size_t len, void *user_arg) {
  (void)user_arg;
  if (s_rx_cb && len > 0) {
    s_rx_cb(data, len);
  }
  return true;  // keep the transfer running
}

static void on_event(const cdc_acm_host_dev_event_data_t *event, void *user_arg) {
  (void)user_arg;
  if (event == NULL) {
    return;
  }
  switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
      ESP_LOGW(TAG, "CDC device disconnected");
      s_dev_lost = true;
      break;
    case CDC_ACM_HOST_SERIAL_STATE:
      ESP_LOGD(TAG, "serial state changed");
      break;
    case CDC_ACM_HOST_ERROR:
      ESP_LOGW(TAG, "CDC host error: %d", event->data.error);
      break;
    default:
      break;
  }
}

// Opens the first candidate that answers. Runs in the link task only.
static esp_err_t try_open(void) {
  const size_t n = sizeof(k_candidates) / sizeof(k_candidates[0]);
  for (size_t i = 0; i < n; i++) {
    const usb_candidate_t *c = &k_candidates[i];
    cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 500,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .event_cb = on_event,
        .data_cb = on_data,
        .user_arg = NULL,
    };

    cdc_acm_dev_hdl_t handle = NULL;
    esp_err_t err = cdc_acm_host_open(c->vid, c->pid, 0, &dev_cfg, &handle);
    if (err != ESP_OK) {
      ESP_LOGD(TAG, "open %04X:%04X (%s) failed: %s", c->vid, c->pid, c->name,
               esp_err_to_name(err));
      continue;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_handle = handle;
    s_dev_lost = false;
    xSemaphoreGive(s_lock);

    // 115200 8N1 is what the slcan firmware expects; the value itself is
    // ignored by the adapter because it is a native USB device.
    cdc_acm_line_coding_t coding = {
        .dwDTERate = 115200,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits = 8,
    };
    esp_err_t lc_err = cdc_acm_host_line_coding_set(handle, &coding);
    esp_err_t cl_err = cdc_acm_host_set_control_line_state(handle, true, true);
    if (lc_err != ESP_OK || cl_err != ESP_OK) {
      ESP_LOGW(TAG, "line coding/control state not accepted (%s / %s)",
               esp_err_to_name(lc_err), esp_err_to_name(cl_err));
    }

    s_connected = true;
    ESP_LOGI(TAG, "adapter opened: %04X:%04X (%s)", c->vid, c->pid, c->name);
    if (s_state_cb) {
      s_state_cb(true);
    }
    return ESP_OK;
  }
  return ESP_ERR_NOT_FOUND;
}

static void close_device(void) {
  cdc_acm_dev_hdl_t handle = NULL;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  handle = s_handle;
  s_handle = NULL;
  s_connected = false;
  s_dev_lost = false;
  xSemaphoreGive(s_lock);

  if (handle != NULL) {
    cdc_acm_host_close(handle);
  }
  ESP_LOGW(TAG, "adapter closed, will retry");
  if (s_state_cb) {
    s_state_cb(false);
  }
}

static void usb_link_task(void *arg) {
  (void)arg;
  for (;;) {
    if (s_handle == NULL) {
      if (try_open() != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2000));
      }
      continue;
    }
    if (s_dev_lost) {
      close_device();
      continue;
    }
    // The class driver reports unplugging through the event callback; a slow
    // poll here only keeps the task from spinning.
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// The CDC-ACM class driver sits on top of the USB Host library: the library
// must be installed first and needs a task pumping its events, otherwise
// cdc_acm_host_install() fails with ESP_ERR_INVALID_STATE and no device ever
// enumerates. (Found on first hardware bring-up: abort loop in app_main.)
static void usb_host_lib_task(void *arg) {
  (void)arg;
  for (;;) {
    uint32_t event_flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
  }
}

esp_err_t usb_cdc_start(usb_cdc_rx_cb_t rx_cb, usb_cdc_state_cb_t state_cb) {
  s_rx_cb = rx_cb;
  s_state_cb = state_cb;

  const usb_host_config_t host_cfg = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  esp_err_t herr = usb_host_install(&host_cfg);
  if (herr != ESP_OK && herr != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(herr));
    return herr;
  }
  if (xTaskCreatePinnedToCore(usb_host_lib_task, "usb_lib", 4096, NULL, 10,
                              NULL, 0) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  s_lock = xSemaphoreCreateMutex();
  if (s_lock == NULL) {
    return ESP_ERR_NO_MEM;
  }

  cdc_acm_host_driver_config_t driver_cfg = {
      .driver_task_stack_size = 4096,
      .driver_task_priority = 10,
      .xCoreID = 0,
  };
  esp_err_t err = cdc_acm_host_install(&driver_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cdc_acm_host_install: %s", esp_err_to_name(err));
    return err;
  }

  if (xTaskCreatePinnedToCore(usb_link_task, "usb_link", 4096, NULL, 6, NULL, 0) !=
      pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "USB host started, waiting for a CDC-ACM adapter");
  return ESP_OK;
}

bool usb_cdc_connected(void) { return s_connected; }

esp_err_t usb_cdc_write(const uint8_t *data, size_t len, uint32_t timeout_ms) {
  if (data == NULL || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_lock == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  cdc_acm_dev_hdl_t handle = s_handle;
  esp_err_t err = ESP_ERR_INVALID_STATE;
  if (handle != NULL) {
    err = cdc_acm_host_data_tx_blocking(handle, data, len, timeout_ms);
  }
  xSemaphoreGive(s_lock);

  if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_TIMEOUT) {
    // The device is gone or its IN endpoint stopped draining. Let the link
    // task close and reopen it instead of queueing commands forever.
    s_dev_lost = true;
  }
  return err;
}

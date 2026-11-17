#include "slcan_relay.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "usb_cdc.h"
#include "ws_server.h"

static const char *TAG = "relay";

#define LINE_MAX CONFIG_RELAY_MAX_LINE_LEN
#define RX_STREAM_SIZE 8192
#define TX_QUEUE_LEN 16
#define TX_GAP_MS 2  // slcan firmware handles one command per USB frame

typedef struct {
  uint16_t len;
  bool raw;
  char data[LINE_MAX + 2];
} tx_item_t;

static StreamBufferHandle_t s_rx_stream;
static QueueHandle_t s_tx_queue;

static volatile bool s_usb_connected;
static volatile uint32_t s_usb_bytes;
static volatile uint32_t s_lines_rx;
static volatile uint32_t s_frames_rx;
static volatile uint32_t s_lines_tx;
static volatile uint32_t s_usb_overflow;
static volatile uint32_t s_ws_dropped;
static volatile uint32_t s_tx_dropped;
static volatile uint32_t s_tx_errors;
static volatile uint32_t s_line_dropped;

static void log_rate_limited(const char *what, uint32_t count) {
  if (count <= 3 || (count % 500) == 0) {
    ESP_LOGW(TAG, "%s (%lu times)", what, (unsigned long)count);
  }
}

// ---------------------------------------------------------------- downlink --

static void deliver_line(const char *line, size_t len, char terminator) {
  char msg[LINE_MAX + 2];
  if (len > LINE_MAX) {
    return;
  }
  if (len > 0) {
    memcpy(msg, line, len);
  }
  msg[len] = terminator;

  s_lines_rx++;
  if (len > 0 && strchr("tTrRdDbB", msg[0]) != NULL) {
    s_frames_rx++;
  }
  if (len >= 2 && msg[0] == 'x' && (msg[1] == 'I' || msg[1] == 'S')) {
    // Adapter info / status replies are useful in the serial log, but not on
    // every poll of the bench page.
    ESP_LOGD(TAG, "adapter: %.*s", (int)len, msg);
  }
  ws_server_broadcast_text(msg, len + 1);
}

static void rx_task(void *arg) {
  (void)arg;
  uint8_t chunk[128];
  char line[LINE_MAX + 2];
  size_t len = 0;
  bool discarding = false;

  for (;;) {
    size_t n = xStreamBufferReceive(s_rx_stream, chunk, sizeof(chunk),
                                    pdMS_TO_TICKS(1000));
    for (size_t i = 0; i < n; i++) {
      const uint8_t b = chunk[i];
      if (b == '\r' || b == '\n' || b == 0x07) {
        if (discarding) {
          discarding = false;
          len = 0;
          continue;
        }
        if (len > 0) {
          deliver_line(line, len, (char)b);
          len = 0;
        } else if (b == 0x07) {
          // A bare BEL is the adapter's "command failed" reply.
          deliver_line(line, 0, 0x07);
        }
        continue;
      }
      if (discarding) {
        continue;
      }
      if (len >= LINE_MAX) {
        s_line_dropped++;
        log_rate_limited("line too long, dropped", s_line_dropped);
        discarding = true;
        len = 0;
        continue;
      }
      line[len++] = (char)b;
    }
  }
}

void relay_on_usb_rx(const uint8_t *data, size_t len) {
  if (data == NULL || len == 0 || s_rx_stream == NULL) {
    return;
  }
  s_usb_bytes += (uint32_t)len;
  // Called from the CDC-ACM driver task: never block here, a short timeout of
  // zero keeps the USB stack responsive. Overflow is counted and the stream is
  // allowed to lose the oldest bytes.
  size_t sent = xStreamBufferSend(s_rx_stream, data, len, 0);
  if (sent < len) {
    s_usb_overflow += (uint32_t)(len - sent);
    log_rate_limited("USB stream buffer full, bytes dropped", s_usb_overflow);
  }
}

void relay_on_usb_state(bool connected) {
  s_usb_connected = connected;
  if (!connected) {
    // Commands queued for the old device would be replayed on the next one;
    // drop them instead.
    if (s_tx_queue != NULL) {
      xQueueReset(s_tx_queue);
    }
  }
  ESP_LOGI(TAG, "USB %s", connected ? "connected" : "disconnected");
  const char *msg = connected ? "#usb up\r" : "#usb down\r";
  ws_server_broadcast_text(msg, strlen(msg));
}

// ------------------------------------------------------------------ uplink --

static void enqueue_tx(const uint8_t *data, size_t len, bool raw) {
  if (s_tx_queue == NULL) {
    return;
  }
  if (len == 0 || len > LINE_MAX) {
    s_tx_dropped++;
    log_rate_limited("uplink command rejected (length)", s_tx_dropped);
    return;
  }
  tx_item_t item;
  item.len = (uint16_t)len;
  item.raw = raw;
  memcpy(item.data, data, len);
  if (xQueueSend(s_tx_queue, &item, 0) != pdTRUE) {
    s_tx_dropped++;
    log_rate_limited("uplink queue full, command dropped", s_tx_dropped);
  }
}

void relay_on_ws_text(const char *data, size_t len) {
  if (data == NULL || len == 0) {
    return;
  }
  // Accept both "O" and "O\r"; the terminator is added once by the TX task.
  while (len > 0 && (data[len - 1] == '\r' || data[len - 1] == '\n')) {
    len--;
  }
  enqueue_tx((const uint8_t *)data, len, false);
}

void relay_on_ws_binary(const uint8_t *data, size_t len) {
  // Reserved: raw bytes are passed through untouched (no CR added).
  enqueue_tx(data, len, true);
}

static void tx_task(void *arg) {
  (void)arg;
  tx_item_t item;
  for (;;) {
    if (xQueueReceive(s_tx_queue, &item, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    size_t len = item.len;
    if (!item.raw) {
      while (len > 0 &&
             (item.data[len - 1] == '\r' || item.data[len - 1] == '\n')) {
        len--;
      }
      item.data[len++] = '\r';
    }
    esp_err_t err = usb_cdc_write((const uint8_t *)item.data, len, 500);
    if (err == ESP_OK) {
      s_lines_tx++;
    } else {
      s_tx_errors++;
      log_rate_limited("USB write failed", s_tx_errors);
    }
    if (!item.raw) {
      vTaskDelay(pdMS_TO_TICKS(TX_GAP_MS));
    }
  }
}

// -------------------------------------------------------------------- setup --

esp_err_t relay_init(void) {
  s_rx_stream = xStreamBufferCreate(RX_STREAM_SIZE, 1);
  if (s_rx_stream == NULL) {
    return ESP_ERR_NO_MEM;
  }
  s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(tx_item_t));
  if (s_tx_queue == NULL) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

esp_err_t relay_start(void) {
  // Keep the relay off the Wi-Fi/USB driver core (0) so a full USB buffer or a
  // Wi-Fi retry does not add latency to line assembly.
  if (xTaskCreatePinnedToCore(rx_task, "slcan_rx", 4096, NULL, 8, NULL, 1) !=
      pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  if (xTaskCreatePinnedToCore(tx_task, "slcan_tx", 4096, NULL, 9, NULL, 1) !=
      pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void relay_note_ws_drop(void) { s_ws_dropped++; }

void relay_get_status(relay_status_t *out) {
  if (out == NULL) {
    return;
  }
  out->usb_connected = s_usb_connected;
  out->usb_bytes = s_usb_bytes;
  out->lines_rx = s_lines_rx;
  out->frames_rx = s_frames_rx;
  out->lines_tx = s_lines_tx;
  out->usb_overflow = s_usb_overflow;
  out->ws_dropped = s_ws_dropped;
  out->tx_dropped = s_tx_dropped;
  out->tx_errors = s_tx_errors;
  out->line_dropped = s_line_dropped;
  out->ws_clients = (uint32_t)ws_server_client_count();
}

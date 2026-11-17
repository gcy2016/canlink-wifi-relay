// slcan line relay: USB CDC <-> WebSocket.
//
// Downlink (adapter -> browser): bytes are assembled into slcan lines and each
// complete line is sent as one WebSocket text message, terminator included
// ('\r' for a normal line, '\a' for a command the adapter rejected).
//
// Uplink (browser -> adapter): each WebSocket text message is one command; the
// relay appends '\r' and paces the writes (the slcan firmware expects one
// command at a time). Binary messages are forwarded verbatim, without a
// terminator, and are reserved for future use.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool usb_connected;
  uint32_t usb_bytes;
  uint32_t lines_rx;
  uint32_t frames_rx;
  uint32_t lines_tx;
  uint32_t usb_overflow;  // stream buffer full: bytes lost between USB and relay
  uint32_t ws_dropped;    // no client / send failure / oversized frame
  uint32_t tx_dropped;    // uplink queue full or command too long
  uint32_t tx_errors;     // USB write failed
  uint32_t line_dropped;  // line longer than CONFIG_RELAY_MAX_LINE_LEN
  uint32_t ws_clients;
} relay_status_t;

// Creates the stream buffer and queues. Call before ws_server_start(), because
// the WebSocket handler can deliver commands as soon as the server is up.
esp_err_t relay_init(void);

// Starts the RX/TX tasks. Call after usb_cdc_start() wired its callbacks.
esp_err_t relay_start(void);

void relay_on_usb_rx(const uint8_t *data, size_t len);
void relay_on_usb_state(bool connected);

void relay_on_ws_text(const char *data, size_t len);
void relay_on_ws_binary(const uint8_t *data, size_t len);

// Bumped by the WebSocket layer when a message cannot be delivered.
void relay_note_ws_drop(void);

void relay_get_status(relay_status_t *out);

#ifdef __cplusplus
}
#endif

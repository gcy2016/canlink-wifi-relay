// HTTP + WebSocket front end.
//
// One TCP port (80) carries:
//   GET  /            small self-test page for bench bring-up
//   GET  /ws          WebSocket, one slcan line per message
//   GET  /api/status  JSON counters (CORS enabled for the future web app)
//   POST /api/wifi    reserved STA provisioning, off by default
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ws_server_start(void);

// Sends one text message to every connected client. Called from the relay RX
// task; safe to call before the server is up (it just drops the data).
void ws_server_broadcast_text(const char *text, size_t len);

// Keepalive; PING frames are answered by the browser automatically.
void ws_server_ping_all(void);

size_t ws_server_client_count(void);

#ifdef __cplusplus
}
#endif

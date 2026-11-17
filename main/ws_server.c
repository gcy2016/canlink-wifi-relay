// HTTP + WebSocket front end for the relay.
//
// The WebSocket carries slcan text only: one message == one slcan line
// (downlink, terminator included) or one slcan command (uplink, terminator
// added by the relay). See docs/protocol.md.

#include "ws_server.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "slcan_relay.h"
#include "wifi_ap.h"

static const char *TAG = "ws";

#define WS_MAX_CLIENTS CONFIG_RELAY_WS_MAX_CLIENTS

static httpd_handle_t s_server;
static int s_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_clients_lock;
static SemaphoreHandle_t s_send_lock;

static void clients_init(void) {
  for (int i = 0; i < WS_MAX_CLIENTS; i++) {
    s_clients[i] = -1;
  }
}

static bool client_add(int fd) {
  bool added = false;
  int used = 0;
  xSemaphoreTake(s_clients_lock, portMAX_DELAY);
  for (int i = 0; i < WS_MAX_CLIENTS; i++) {
    if (s_clients[i] == fd) {
      added = true;
    }
  }
  if (!added) {
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
      if (s_clients[i] < 0) {
        s_clients[i] = fd;
        added = true;
        break;
      }
    }
  }
  for (int i = 0; i < WS_MAX_CLIENTS; i++) {
    if (s_clients[i] >= 0) {
      used++;
    }
  }
  xSemaphoreGive(s_clients_lock);
  if (added) {
    ESP_LOGI(TAG, "WebSocket client fd=%d connected (%d/%d)", fd, used,
             WS_MAX_CLIENTS);
  }
  return added;
}

static void client_remove(int fd) {
  bool removed = false;
  xSemaphoreTake(s_clients_lock, portMAX_DELAY);
  for (int i = 0; i < WS_MAX_CLIENTS; i++) {
    if (s_clients[i] == fd) {
      s_clients[i] = -1;
      removed = true;
      break;
    }
  }
  xSemaphoreGive(s_clients_lock);
  if (removed) {
    ESP_LOGI(TAG, "WebSocket client fd=%d gone", fd);
  }
}

size_t ws_server_client_count(void) {
  size_t n = 0;
  if (s_clients_lock == NULL) {
    return 0;
  }
  xSemaphoreTake(s_clients_lock, portMAX_DELAY);
  for (int i = 0; i < WS_MAX_CLIENTS; i++) {
    if (s_clients[i] >= 0) {
      n++;
    }
  }
  xSemaphoreGive(s_clients_lock);
  return n;
}

void ws_server_broadcast_text(const char *text, size_t len) {
  if (s_server == NULL || s_send_lock == NULL || text == NULL || len == 0) {
    return;
  }

  int fds[WS_MAX_CLIENTS];
  size_t n = 0;
  xSemaphoreTake(s_clients_lock, portMAX_DELAY);
  for (int i = 0; i < WS_MAX_CLIENTS; i++) {
    if (s_clients[i] >= 0) {
      fds[n++] = s_clients[i];
    }
  }
  xSemaphoreGive(s_clients_lock);
  if (n == 0) {
    return;
  }

  httpd_ws_frame_t frame = {
      .final = true,
      .fragmented = false,
      .type = HTTPD_WS_TYPE_TEXT,
      .payload = (uint8_t *)text,
      .len = len,
  };

  // httpd_ws_send_frame_async() writes to the socket, so serialise senders to
  // keep frames from different tasks from interleaving.
  xSemaphoreTake(s_send_lock, portMAX_DELAY);
  for (size_t i = 0; i < n; i++) {
    if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
      client_remove(fds[i]);
      continue;
    }
    esp_err_t err = httpd_ws_send_frame_async(s_server, fds[i], &frame);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "send to fd=%d failed: %s", fds[i], esp_err_to_name(err));
      relay_note_ws_drop();
      client_remove(fds[i]);
    }
  }
  xSemaphoreGive(s_send_lock);
}

void ws_server_ping_all(void) {
  if (s_server == NULL) {
    return;
  }
  httpd_ws_frame_t ping = {
      .final = true,
      .fragmented = false,
      .type = HTTPD_WS_TYPE_PING,
      .payload = NULL,
      .len = 0,
  };
  int fds[WS_MAX_CLIENTS];
  size_t n = 0;
  xSemaphoreTake(s_clients_lock, portMAX_DELAY);
  for (int i = 0; i < WS_MAX_CLIENTS; i++) {
    if (s_clients[i] >= 0) {
      fds[n++] = s_clients[i];
    }
  }
  xSemaphoreGive(s_clients_lock);

  xSemaphoreTake(s_send_lock, portMAX_DELAY);
  for (size_t i = 0; i < n; i++) {
    if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
      client_remove(fds[i]);
      continue;
    }
    if (httpd_ws_send_frame_async(s_server, fds[i], &ping) != ESP_OK) {
      client_remove(fds[i]);
    }
  }
  xSemaphoreGive(s_send_lock);
}

static esp_err_t ws_handler(httpd_req_t *req) {
  const int fd = httpd_req_to_sockfd(req);

  // First call is the HTTP upgrade request; nothing to read yet.
  if (req->method == HTTP_GET) {
    if (!client_add(fd)) {
      ESP_LOGW(TAG, "no free WebSocket slot, closing fd=%d", fd);
      httpd_sess_trigger_close(req->handle, fd);
    }
    return ESP_OK;
  }

  uint8_t buf[CONFIG_RELAY_MAX_LINE_LEN + 1];
  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.type = HTTPD_WS_TYPE_TEXT;

  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) {
    client_remove(fd);
    return err;
  }
  if (frame.len > CONFIG_RELAY_MAX_LINE_LEN) {
    // Reading the payload is not worth it; the peer is speaking another
    // protocol or sent a huge burst. Drop the session.
    ESP_LOGW(TAG, "oversized WS frame (%u bytes), closing fd=%d",
             (unsigned)frame.len, fd);
    relay_note_ws_drop();
    client_remove(fd);
    return ESP_FAIL;
  }
  if (frame.len > 0) {
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
      client_remove(fd);
      return err;
    }
  }

  switch (frame.type) {
    case HTTPD_WS_TYPE_TEXT:
      relay_on_ws_text((const char *)buf, frame.len);
      break;
    case HTTPD_WS_TYPE_BINARY:
      relay_on_ws_binary(buf, frame.len);
      break;
    case HTTPD_WS_TYPE_CLOSE:
      client_remove(fd);
      break;
    default:
      break;
  }
  return ESP_OK;
}

static void set_cors(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t status_get(httpd_req_t *req) {
  relay_status_t st;
  relay_get_status(&st);

  char ap_ip[16] = "0.0.0.0";
  wifi_get_ap_ip(ap_ip, sizeof(ap_ip));

  char body[640];
  int n = snprintf(
      body, sizeof(body),
      "{\"usb\":%s,\"frames_rx\":%lu,\"lines_tx\":%lu,\"lines_rx\":%lu,"
      "\"drops\":{\"usb\":%lu,\"ws\":%lu,\"tx\":%lu,\"line\":%lu},"
      "\"ws_clients\":%u,\"ap\":{\"up\":%s,\"ssid\":\"%s\",\"ip\":\"%s\"},"
      "\"sta\":{\"up\":%s},\"heap\":%lu,\"uptime_s\":%lu}\n",
      st.usb_connected ? "true" : "false", (unsigned long)st.frames_rx,
      (unsigned long)st.lines_tx, (unsigned long)st.lines_rx,
      (unsigned long)st.usb_overflow, (unsigned long)st.ws_dropped,
      (unsigned long)st.tx_dropped, (unsigned long)st.line_dropped,
      (unsigned)ws_server_client_count(), wifi_ap_up() ? "true" : "false",
      CONFIG_RELAY_AP_SSID, ap_ip, wifi_sta_up() ? "true" : "false",
      (unsigned long)esp_get_free_heap_size(),
      (unsigned long)(esp_timer_get_time() / 1000000));

  set_cors(req);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, strlen(body));
}

static esp_err_t options_handler(httpd_req_t *req) {
  set_cors(req);
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, NULL, 0);
}

#if CONFIG_RELAY_ENABLE_STA_PROVISIONING
// Minimal "ssid":"..." extractor for the reserved provisioning endpoint.
// Only plain ASCII without quotes/backslashes is accepted; this is a bring-up
// hook, not a general JSON parser.
static bool json_string(const char *body, const char *key, char *out,
                        size_t out_len) {
  char pattern[32];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(body, pattern);
  if (p == NULL) {
    return false;
  }
  p += strlen(pattern);
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (*p != ':') {
    return false;
  }
  p++;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (*p != '"') {
    return false;
  }
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < out_len) {
    out[i++] = *p++;
  }
  out[i] = '\0';
  return *p == '"';
}

static esp_err_t wifi_post(httpd_req_t *req) {
  char body[256];
  int len = req->content_len;
  if (len <= 0 || len >= (int)sizeof(body)) {
    set_cors(req);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
    return ESP_OK;
  }
  int got = httpd_req_recv(req, body, len);
  if (got != len) {
    set_cors(req);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");
    return ESP_OK;
  }
  body[len] = '\0';

  char ssid[33];
  char pass[65];
  if (!json_string(body, "ssid", ssid, sizeof(ssid))) {
    set_cors(req);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
    return ESP_OK;
  }
  if (!json_string(body, "pass", pass, sizeof(pass))) {
    pass[0] = '\0';
  }

  esp_err_t err = wifi_set_sta_credentials(ssid, pass);
  set_cors(req);
  httpd_resp_set_type(req, "application/json");
  if (err != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               esp_err_to_name(err));
  }
  return httpd_resp_sendstr(req, "{\"ok\":true,\"reboot_required\":true}\n");
}
#endif  // CONFIG_RELAY_ENABLE_STA_PROVISIONING

// Bench page: shows /api/status and a raw slcan console over the same
// WebSocket the real web app will use. Deliberately dependency-free.
static const char k_index_html[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>CANLink relay</title><style>"
    "body{font:14px/1.4 system-ui,sans-serif;margin:16px;max-width:760px}"
    "h1{font-size:18px}pre{background:#111;color:#0f0;padding:10px;height:45vh;"
    "overflow:auto;white-space:pre-wrap;word-break:break-all}"
    "button{margin:2px;padding:6px 10px}input{width:60%;padding:6px}"
    ".k{color:#555}.bad{color:#b00}</style></head><body>"
    "<h1>CANLink Wi-Fi relay</h1>"
    "<div id=\"st\">loading...</div>"
    "<p><button data-cmd=\"xI\">xI info</button>"
    "<button data-cmd=\"xS\">xS status</button>"
    "<button data-cmd=\"O\">O open</button>"
    "<button data-cmd=\"C\">C close</button>"
    "<button data-cmd=\"S6\">S6 500k</button></p>"
    "<p><input id=\"cmd\" placeholder=\"slcan command, no CR\">"
    "<button id=\"send\">Send</button></p>"
    "<pre id=\"log\"></pre><script>"
    "var log=document.getElementById('log');"
    "function add(s){log.textContent+=s+'\\n';"
    "if(log.textContent.length>20000)log.textContent=log.textContent.slice(-10000);"
    "log.scrollTop=log.scrollHeight;}"
    "function render(s){document.getElementById('st').innerHTML="
    "'<span class=\"k\">USB</span> '+(s.usb?'<b>up</b>':'<span class=\"bad\">down</span>')"
    "+' &nbsp; <span class=\"k\">AP</span> '+s.ap.ssid+' '+s.ap.ip"
    "+' &nbsp; <span class=\"k\">frames</span> '+s.frames_rx"
    "+' &nbsp; <span class=\"k\">drops</span> usb '+s.drops.usb+' / tx '+s.drops.tx"
    "+' / ws '+s.drops.ws+' &nbsp; <span class=\"k\">heap</span> '+s.heap;}"
    "function poll(){fetch('/api/status').then(function(r){return r.json()})"
    ".then(render).catch(function(){});}"
    "setInterval(poll,2000);poll();"
    "var ws,retry=1000;"
    "function connect(){ws=new WebSocket('ws://'+location.host+'/ws');"
    "ws.onopen=function(){retry=1000;add('-- ws open --');};"
    "ws.onmessage=function(e){add(e.data.replace(/\\r/g,'\\\\r').replace(/\\u0007/g,'<BEL>'));};"
    "ws.onclose=function(){add('-- ws closed, retry '+retry+' ms --');"
    "setTimeout(connect,retry);retry=Math.min(retry*2,5000);};"
    "ws.onerror=function(){try{ws.close()}catch(e){}};}"
    "connect();"
    "function send(c){if(ws&&ws.readyState===1){ws.send(c);add('> '+c);}}"
    "document.getElementById('send').onclick=function(){"
    "var i=document.getElementById('cmd');send(i.value.trim());i.value='';};"
    "document.querySelectorAll('button[data-cmd]').forEach(function(b){"
    "b.onclick=function(){send(b.getAttribute('data-cmd'));};});"
    "</script></body></html>";

static esp_err_t index_get(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, k_index_html, HTTPD_RESP_USE_STRLEN);
}

esp_err_t ws_server_start(void) {
  s_clients_lock = xSemaphoreCreateMutex();
  s_send_lock = xSemaphoreCreateMutex();
  if (s_clients_lock == NULL || s_send_lock == NULL) {
    return ESP_ERR_NO_MEM;
  }
  clients_init();

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_open_sockets = WS_MAX_CLIENTS + 3;
  config.lru_purge_enable = true;
  config.stack_size = 8192;
  config.task_priority = 5;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
    s_server = NULL;
    return err;
  }

  const httpd_uri_t index_uri = {
      .uri = "/", .method = HTTP_GET, .handler = index_get, .user_ctx = NULL};
  const httpd_uri_t ws_uri = {
      .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .user_ctx = NULL,
      .is_websocket = true};
  const httpd_uri_t status_uri = {
      .uri = "/api/status", .method = HTTP_GET, .handler = status_get,
      .user_ctx = NULL};
  const httpd_uri_t status_opt = {
      .uri = "/api/status", .method = HTTP_OPTIONS, .handler = options_handler,
      .user_ctx = NULL};

  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &index_uri), TAG,
                      "index");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &ws_uri), TAG, "ws");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &status_uri), TAG,
                      "status");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &status_opt), TAG,
                      "status options");

#if CONFIG_RELAY_ENABLE_STA_PROVISIONING
  const httpd_uri_t wifi_uri = {
      .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post,
      .user_ctx = NULL};
  const httpd_uri_t wifi_opt = {
      .uri = "/api/wifi", .method = HTTP_OPTIONS, .handler = options_handler,
      .user_ctx = NULL};
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &wifi_uri), TAG,
                      "wifi");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &wifi_opt), TAG,
                      "wifi options");
#endif

  ESP_LOGI(TAG, "HTTP/WebSocket listening on port 80 (/ws)");
  return ESP_OK;
}

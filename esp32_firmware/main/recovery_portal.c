#include "recovery_portal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RecoveryPortal";

#define RECOVERY_AP_SSID "ESP32-Setup"
#define RECOVERY_AP_PASS "esp32sysmon"
#define RECOVERY_AP_CHANNEL 1

static httpd_handle_t server;
static bool portal_started;
static esp_netif_t *ap_netif;
static char ap_ip[16] = "192.168.4.1";

static const char *const html_page =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 SysMon Wi-Fi</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#0b1020;color:#e8eefc;margin:0;padding:24px;}"
    ".card{max-width:720px;margin:0 auto;background:#121a31;border:1px solid #233055;border-radius:16px;padding:24px;}"
    "label{display:block;margin-top:12px;font-weight:600;}"
    "input{width:100%;box-sizing:border-box;margin-top:6px;padding:12px;border-radius:10px;border:1px solid #334062;background:#0a0f1f;color:#e8eefc;}"
    "button{margin-top:16px;background:#4f7cff;color:#fff;border:0;border-radius:10px;padding:12px 16px;font-weight:700;cursor:pointer;}"
    "code{background:#0a0f1f;padding:2px 6px;border-radius:6px;}"
    ".muted{color:#a8b3d6;}"
    "</style></head><body><div class='card'>"
    "<h1>ESP32 SysMon Wi-Fi setup</h1>"
    "<p class='muted'>Configure the ESP32 to join the same Wi-Fi as the PC. The PC service will then list this board from the LAN and push telemetry over HTTP.</p>"
    "<p>Status: <code id='state'>...</code><br>Station IP: <code id='station-ip'>--</code><br>AP IP: <code id='ap-ip'>--</code></p>"
    "<form id='wifi-form'>"
    "<label>Wi-Fi SSID<input name='ssid' id='ssid' autocomplete='off' required></label>"
    "<label>Wi-Fi password<input name='password' id='password' type='password' autocomplete='off'></label>"
    "<button type='submit'>Save and connect</button>"
    "</form>"
    "<p id='message' class='muted'></p>"
    "<script>"
    "const stateEl=document.getElementById('state');"
    "const stationIpEl=document.getElementById('station-ip');"
    "const apIpEl=document.getElementById('ap-ip');"
    "const messageEl=document.getElementById('message');"
    "const form=document.getElementById('wifi-form');"
    "async function refresh(){"
    "try{"
    "const r=await fetch('/api/info',{cache:'no-store'});"
    "const j=await r.json();"
    "stateEl.textContent=j.state;"
    "stationIpEl.textContent=j.station_ip;"
    "apIpEl.textContent=j.ap_ip;"
    "document.getElementById('ssid').value=j.wifi_ssid||'';"
    "messageEl.textContent=j.telemetry ? 'Telemetry is being received.' : 'Waiting for PC telemetry.';"
    "}catch(e){messageEl.textContent='Unable to refresh status: '+e.message;}"
    "}"
    "form.addEventListener('submit',async e=>{"
    "e.preventDefault();"
    "const body=new URLSearchParams(new FormData(form));"
    "try{"
    "const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});"
    "const j=await r.json();"
    "messageEl.textContent=j.message;"
    "await refresh();"
    "}catch(e){messageEl.textContent='Save failed: '+e.message;}"
    "});"
    "refresh();setInterval(refresh,2000);"
    "</script></div></body></html>";

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len == 0) {
        return ESP_FAIL;
    }

    size_t total_len = (size_t)req->content_len;
    if (total_len + 1U > buf_size) {
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    while (offset < total_len) {
        int received = httpd_req_recv(req, buf + offset, total_len - offset);
        if (received <= 0) {
            return ESP_FAIL;
        }
        offset += (size_t)received;
    }
    buf[offset] = '\0';
    return ESP_OK;
}

static bool json_append_char(char *dst, size_t dst_size, size_t *offset, char c)
{
    if (*offset + 1U >= dst_size) {
        return false;
    }
    dst[(*offset)++] = c;
    dst[*offset] = '\0';
    return true;
}

static bool json_append_text(char *dst, size_t dst_size, size_t *offset, const char *text)
{
    while (*text != '\0') {
        if (!json_append_char(dst, dst_size, offset, *text++)) {
            return false;
        }
    }
    return true;
}

static bool json_append_string(char *dst, size_t dst_size, size_t *offset, const char *value)
{
    if (!json_append_char(dst, dst_size, offset, '"')) {
        return false;
    }

    if (value == NULL) {
        value = "";
    }
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\') {
            if (!json_append_char(dst, dst_size, offset, '\\') ||
                !json_append_char(dst, dst_size, offset, (char)*p)) {
                return false;
            }
        } else if (*p < 0x20U) {
            if (*offset + 7U >= dst_size) {
                return false;
            }
            int written = snprintf(dst + *offset, dst_size - *offset, "\\u%04x", (unsigned)*p);
            if (written != 6) {
                return false;
            }
            *offset += (size_t)written;
        } else if (!json_append_char(dst, dst_size, offset, (char)*p)) {
            return false;
        }
    }

    return json_append_char(dst, dst_size, offset, '"');
}

static void url_decode(char *dst, size_t dst_size, const char *src)
{
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1U < dst_size; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '+') {
            dst[out++] = ' ';
            continue;
        }
        if (c == '%' && src[i + 1] != '\0' && src[i + 2] != '\0' &&
            isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[out++] = (char)strtol(hex, NULL, 16);
            i += 2;
            continue;
        }
        dst[out++] = (char)c;
    }
    dst[out] = '\0';
}

static esp_err_t extract_field(const char *body, const char *name, char *out, size_t out_size)
{
    char key[32];
    snprintf(key, sizeof(key), "%s=", name);
    const char *start = strstr(body, key);
    if (start == NULL) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return ESP_ERR_NOT_FOUND;
    }

    start += strlen(key);
    const char *end = strchr(start, '&');
    size_t len = end != NULL ? (size_t)(end - start) : strlen(start);
    if (len >= 128U) {
        len = 127U;
    }

    char raw[128];
    memcpy(raw, start, len);
    raw[len] = '\0';
    url_decode(out, out_size, raw);
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    char json[512];
    size_t offset = 0;
    int written;
    bool ok =
        json_append_text(json, sizeof(json), &offset, "{\"device\":") &&
        json_append_string(json, sizeof(json), &offset, wifi_transport_device_name()) &&
        json_append_text(json, sizeof(json), &offset, ",\"state\":") &&
        json_append_string(json, sizeof(json), &offset, wifi_transport_state_name()) &&
        json_append_text(json, sizeof(json), &offset, ",\"retry\":");
    if (ok) {
        written = snprintf(json + offset, sizeof(json) - offset, "%u", (unsigned)wifi_transport_retry_count());
        ok = written > 0 && (size_t)written < sizeof(json) - offset;
        if (ok) {
            offset += (size_t)written;
        }
    }
    ok = ok &&
        json_append_text(json, sizeof(json), &offset, ",\"telemetry\":") &&
        json_append_text(json, sizeof(json), &offset, wifi_transport_has_telemetry() ? "true" : "false") &&
        json_append_text(json, sizeof(json), &offset, ",\"wifi_ssid\":") &&
        json_append_string(json, sizeof(json), &offset, wifi_transport_sta_ssid()) &&
        json_append_text(json, sizeof(json), &offset, ",\"station_ip\":") &&
        json_append_string(json, sizeof(json), &offset, wifi_transport_sta_ip()) &&
        json_append_text(json, sizeof(json), &offset, ",\"ap_ssid\":") &&
        json_append_string(json, sizeof(json), &offset, wifi_transport_ap_ssid()) &&
        json_append_text(json, sizeof(json), &offset, ",\"ap_ip\":") &&
        json_append_string(json, sizeof(json), &offset, wifi_transport_ap_ip()) &&
        json_append_text(json, sizeof(json), &offset, "}");

    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build info");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char body[256];
    char ssid[33];
    char password[65];
    esp_err_t err = read_body(req, body, sizeof(body));
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_OK;
    }
    if (extract_field(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_OK;
    }
    if (extract_field(body, "password", password, sizeof(password)) != ESP_OK) {
        password[0] = '\0';
    }
    err = wifi_transport_apply_credentials(ssid, password);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save Wi-Fi credentials");
        return ESP_OK;
    }

    char json[128];
    size_t offset = 0;
    bool ok =
        json_append_text(json, sizeof(json), &offset, "{\"message\":\"Saved Wi-Fi credentials\"") &&
        json_append_text(json, sizeof(json), &offset, "}");
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build response");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t telemetry_post_handler(httpd_req_t *req)
{
    sys_status_t packet;
    if (req->content_len != (int)sizeof(packet)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected 12-byte telemetry packet");
        return ESP_OK;
    }

    size_t offset = 0;
    while (offset < sizeof(packet)) {
        int received = httpd_req_recv(req, ((char *)&packet) + offset, sizeof(packet) - offset);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read telemetry");
            return ESP_OK;
        }
        offset += (size_t)received;
    }

    esp_err_t err = wifi_transport_submit_telemetry(&packet);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Telemetry queue unavailable");
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    if (server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t info = {
        .uri = "/api/info",
        .method = HTTP_GET,
        .handler = info_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t wifi = {
        .uri = "/api/wifi",
        .method = HTTP_POST,
        .handler = wifi_post_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t telemetry = {
        .uri = "/api/telemetry",
        .method = HTTP_POST,
        .handler = telemetry_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &info);
    httpd_register_uri_handler(server, &wifi);
    httpd_register_uri_handler(server, &telemetry);
    return ESP_OK;
}

esp_err_t recovery_portal_start_server(void)
{
    return start_http_server();
}

esp_err_t recovery_portal_start(void)
{
    if (!portal_started) {
        if (ap_netif == NULL) {
            ap_netif = esp_netif_create_default_wifi_ap();
            if (ap_netif == NULL) {
                ESP_LOGE(TAG, "Failed to create AP netif");
                return ESP_FAIL;
            }
        }

        wifi_config_t wifi_config = {
            .ap = {
                .ssid = RECOVERY_AP_SSID,
                .ssid_len = 0,
                .channel = RECOVERY_AP_CHANNEL,
                .password = RECOVERY_AP_PASS,
                .max_connection = 4,
                .authmode = WIFI_AUTH_WPA2_PSK,
            },
        };
        if (strlen(RECOVERY_AP_PASS) == 0) {
            wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        }

        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to switch to APSTA mode: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure AP: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STOPPED) {
            ESP_LOGE(TAG, "Failed to start Wi-Fi: %s", esp_err_to_name(err));
            return err;
        }

        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            snprintf(ap_ip, sizeof(ap_ip), IPSTR, IP2STR(&ip_info.ip));
        }

        err = start_http_server();
        if (err != ESP_OK) {
            return err;
        }

        portal_started = true;
        ESP_LOGW(TAG, "Config portal ready on ssid=%s ip=%s", RECOVERY_AP_SSID, ap_ip);
    }

    return ESP_OK;
}

esp_err_t recovery_portal_stop_ap(void)
{
    if (!portal_started) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to STA-only mode: %s", esp_err_to_name(err));
        return err;
    }

    portal_started = false;
    snprintf(ap_ip, sizeof(ap_ip), "0.0.0.0");
    ESP_LOGI(TAG, "Config AP stopped; Wi-Fi is STA-only");
    return ESP_OK;
}

bool recovery_portal_is_active(void)
{
    return portal_started;
}

const char *recovery_portal_ssid(void)
{
    return RECOVERY_AP_SSID;
}

const char *recovery_portal_ip(void)
{
    return ap_ip;
}

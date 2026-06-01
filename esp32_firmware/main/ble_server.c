#include "ble_server.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "freertos/task.h"

#include "recovery_portal.h"

static const char *TAG = "WiFiTransport";
static const char *NVS_NAMESPACE = "wifi";
static const char *NVS_KEY_SSID = "ssid";
static const char *NVS_KEY_PASS = "pass";
static const char *DEFAULT_STA_SSID = "4G CPE E0F8";
static const char *DEFAULT_STA_PASS = "88888888";
static const char *DEVICE_NAME = "ESP32-SysMon";
static const uint8_t MAX_WIFI_RETRIES = 5U;

QueueHandle_t telemetry_queue = NULL;

static volatile wifi_transport_state_t transport_state = WIFI_TRANSPORT_STATE_BOOT;
static volatile uint8_t retry_count;
static volatile bool telemetry_seen;
static bool credentials_loaded;
static char saved_ssid[33];
static char saved_pass[65];
static char connected_ssid[33];
static char sta_ip[16] = "0.0.0.0";

static esp_netif_t *sta_netif;
static esp_event_handler_instance_t wifi_event_any_id;
static esp_event_handler_instance_t ip_event_got_ip;

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static esp_err_t configure_sta_credentials(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = { 0 };
    copy_string((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), ssid);
    copy_string((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), password == NULL ? "" : password);
    wifi_config.sta.threshold.authmode = (password != NULL && password[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static void set_state(wifi_transport_state_t state)
{
    transport_state = state;
}

static esp_err_t load_credentials_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        credentials_loaded = false;
        saved_ssid[0] = '\0';
        saved_pass[0] = '\0';
        return err;
    }

    size_t ssid_len = sizeof(saved_ssid);
    size_t pass_len = sizeof(saved_pass);
    err = nvs_get_str(handle, NVS_KEY_SSID, saved_ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, NVS_KEY_PASS, saved_pass, &pass_len);
    }
    nvs_close(handle);

    if (err == ESP_OK && saved_ssid[0] != '\0') {
        credentials_loaded = true;
        copy_string(connected_ssid, sizeof(connected_ssid), saved_ssid);
        return ESP_OK;
    }

    credentials_loaded = true;
    copy_string(saved_ssid, sizeof(saved_ssid), DEFAULT_STA_SSID);
    copy_string(saved_pass, sizeof(saved_pass), DEFAULT_STA_PASS);
    copy_string(connected_ssid, sizeof(connected_ssid), DEFAULT_STA_SSID);
    return ESP_OK;
}

static esp_err_t save_credentials_to_nvs(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PASS, password == NULL ? "" : password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (credentials_loaded) {
            set_state(WIFI_TRANSPORT_STATE_CONNECTING);
        } else {
            set_state(WIFI_TRANSPORT_STATE_AP_CONFIG);
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *event = (const wifi_event_sta_connected_t *)event_data;
        if (event != NULL) {
            size_t len = event->ssid_len;
            if (len >= sizeof(connected_ssid)) {
                len = sizeof(connected_ssid) - 1U;
            }
            memcpy(connected_ssid, event->ssid, len);
            connected_ssid[len] = '\0';
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < MAX_WIFI_RETRIES && credentials_loaded) {
            retry_count++;
            set_state(WIFI_TRANSPORT_STATE_CONNECTING);
            ESP_LOGW(TAG, "Wi-Fi disconnected; retry %u/%u", (unsigned)retry_count, (unsigned)MAX_WIFI_RETRIES);
            esp_wifi_connect();
        } else {
            set_state(WIFI_TRANSPORT_STATE_AP_CONFIG);
            ESP_LOGW(TAG, "Wi-Fi connect retries exhausted; opening config AP");
            esp_err_t err = recovery_portal_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open config AP: %s", esp_err_to_name(err));
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (event != NULL) {
            snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        }
        retry_count = 0;
        set_state(WIFI_TRANSPORT_STATE_CONNECTED);
        esp_err_t err = recovery_portal_stop_ap();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Connected, but failed to stop config AP: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "Connected to Wi-Fi, IP=%s", sta_ip);
    }
}

esp_err_t wifi_transport_submit_telemetry(const sys_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    telemetry_seen = true;
    if (telemetry_queue != NULL) {
        xQueueOverwrite(telemetry_queue, status);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t wifi_transport_apply_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = save_credentials_to_nvs(ssid, password == NULL ? "" : password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save Wi-Fi credentials: %s", esp_err_to_name(err));
        return err;
    }

    copy_string(saved_ssid, sizeof(saved_ssid), ssid);
    copy_string(saved_pass, sizeof(saved_pass), password == NULL ? "" : password);
    copy_string(connected_ssid, sizeof(connected_ssid), ssid);
    credentials_loaded = true;
    retry_count = 0;
    set_state(WIFI_TRANSPORT_STATE_CONNECTING);

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable STA while config AP is active: %s", esp_err_to_name(err));
        return err;
    }

    err = configure_sta_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply STA config: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Wi-Fi connect: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

wifi_transport_state_t wifi_transport_state(void)
{
    return transport_state;
}

const char *wifi_transport_state_name(void)
{
    switch (transport_state) {
        case WIFI_TRANSPORT_STATE_CONNECTING:
            return "connecting";
        case WIFI_TRANSPORT_STATE_CONNECTED:
            return "connected";
        case WIFI_TRANSPORT_STATE_AP_CONFIG:
            return "ap_config";
        case WIFI_TRANSPORT_STATE_BOOT:
        default:
            return "boot";
    }
}

uint8_t wifi_transport_retry_count(void)
{
    return retry_count;
}

bool wifi_transport_has_telemetry(void)
{
    return telemetry_seen;
}

bool wifi_transport_is_station_connected(void)
{
    return transport_state == WIFI_TRANSPORT_STATE_CONNECTED;
}

const char *wifi_transport_sta_ip(void)
{
    return sta_ip;
}

const char *wifi_transport_ap_ip(void)
{
    return recovery_portal_is_active() ? recovery_portal_ip() : "0.0.0.0";
}

const char *wifi_transport_ap_ssid(void)
{
    return recovery_portal_is_active() ? recovery_portal_ssid() : "";
}

const char *wifi_transport_sta_ssid(void)
{
    return connected_ssid;
}

const char *wifi_transport_device_name(void)
{
    return DEVICE_NAME;
}

esp_err_t wifi_transport_init(void)
{
    if (telemetry_queue == NULL) {
        telemetry_queue = xQueueCreate(1, sizeof(sys_status_t));
        if (telemetry_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create telemetry queue");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = load_credentials_from_nvs();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load Wi-Fi credentials: %s", esp_err_to_name(err));
    }

    if (sta_netif == NULL) {
        sta_netif = esp_netif_create_default_wifi_sta();
        if (sta_netif == NULL) {
            ESP_LOGE(TAG, "Failed to create STA netif");
            return ESP_FAIL;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &wifi_event_any_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register Wi-Fi handler: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &ip_event_got_ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP handler: %s", esp_err_to_name(err));
        return err;
    }

    err = recovery_portal_start_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    if (credentials_loaded) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set STA mode: %s", esp_err_to_name(err));
            return err;
        }

        err = configure_sta_credentials(saved_ssid, saved_pass);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to seed STA config: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STOPPED) {
            ESP_LOGE(TAG, "Failed to start Wi-Fi: %s", esp_err_to_name(err));
            return err;
        }

        copy_string(connected_ssid, sizeof(connected_ssid), saved_ssid);
        set_state(WIFI_TRANSPORT_STATE_CONNECTING);
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Initial Wi-Fi connect failed: %s", esp_err_to_name(err));
            set_state(WIFI_TRANSPORT_STATE_AP_CONFIG);
            err = recovery_portal_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open config AP: %s", esp_err_to_name(err));
                return err;
            }
        }
    } else {
        set_state(WIFI_TRANSPORT_STATE_AP_CONFIG);
        err = recovery_portal_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open config AP: %s", esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}

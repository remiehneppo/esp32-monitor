#ifndef BLE_SERVER_H
#define BLE_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct __attribute__((packed)) {
    uint8_t cpu_usage;
    uint8_t ram_usage;
    uint8_t gpu_usage;
    uint8_t vram_usage;
    uint32_t net_up_kbps;
    uint32_t net_down_kbps;
} sys_status_t;

typedef enum {
    WIFI_TRANSPORT_STATE_BOOT = 0,
    WIFI_TRANSPORT_STATE_CONNECTING,
    WIFI_TRANSPORT_STATE_CONNECTED,
    WIFI_TRANSPORT_STATE_AP_CONFIG,
} wifi_transport_state_t;

extern QueueHandle_t telemetry_queue;

esp_err_t wifi_transport_init(void);
esp_err_t wifi_transport_apply_credentials(const char *ssid, const char *password);
esp_err_t wifi_transport_submit_telemetry(const sys_status_t *status);

wifi_transport_state_t wifi_transport_state(void);
const char *wifi_transport_state_name(void);
uint8_t wifi_transport_retry_count(void);
bool wifi_transport_has_telemetry(void);
bool wifi_transport_is_station_connected(void);
const char *wifi_transport_sta_ip(void);
const char *wifi_transport_ap_ip(void);
const char *wifi_transport_ap_ssid(void);
const char *wifi_transport_sta_ssid(void);
const char *wifi_transport_device_name(void);

static inline esp_err_t ble_server_init(void)
{
    return wifi_transport_init();
}

static inline uint8_t ble_server_retry_count(void)
{
    return wifi_transport_retry_count();
}

static inline bool ble_server_has_telemetry(void)
{
    return wifi_transport_has_telemetry();
}

#endif // BLE_SERVER_H

idf_component_register(SRCS "main.c" "display_ssd1306.c" "ble_server.c" "recovery_portal.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash driver esp_wifi esp_netif esp_event esp_http_server)

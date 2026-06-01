idf_component_register(
    SRCS
        "main.cpp"
        "renderer.cpp"
        "display_interface.cpp"
        "display_ssd1306.cpp"
        "display_st7789.cpp"
        "ble_server.c"
        "recovery_portal.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash driver esp_wifi esp_netif esp_event esp_http_server
)

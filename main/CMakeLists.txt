idf_component_register(
    SRCS "main.c" "web_ui.c"
    REQUIRES px_system drv_rgb_led board_devkitc1
             esp_http_server esp_wifi esp_netif nvs_flash
)

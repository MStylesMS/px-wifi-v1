idf_component_register(
    SRCS "prop_engine.c" "main.c" "web_ui.c" "web_ui_json.c"
    REQUIRES px_system drv_rgb_led board_devkitc1 svc_wifi svc_mqtt lib_json_helper lib_http_proxy drv_battery_monitor svc_nvs_config svc_ota
             esp_http_server esp_wifi esp_netif nvs_flash esp_app_format esp_timer spiffs mdns app_update
             esp_adc esp_driver_tsens esp_driver_gpio esp_driver_i2c esp_driver_ledc mbedtls espressif__cjson
    EMBED_TXTFILES "webui/index.html" "webui/config.html" "webui/connection.html" "webui/update.html" "webui/styles.css" "webui/app.js"
    EMBED_FILES "webui/assets/logo.png"
)

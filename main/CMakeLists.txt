idf_component_register(
    SRCS "prop_engine.c" "main.c" "web_ui.c" "web_ui_json.c"
    REQUIRES px_system drv_rgb_led board_devkitc1
             esp_http_server esp_wifi esp_netif nvs_flash esp_app_format esp_timer spiffs mdns mqtt driver app_update json
    EMBED_TXTFILES "webui/index.html" "webui/config.html" "webui/connection.html" "webui/update.html" "webui/styles.css" "webui/app.js"
    EMBED_FILES "webui/assets/logo.png"
)

#include "web_ui.h"

#include <string.h>
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "web_ui";

static const char *INDEX_HTML =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PX-WiFi-V1</title>"
    "<style>body{font-family:system-ui;padding:1.25rem;max-width:48rem;margin:auto;}"
    "pre{background:#f4f4f4;padding:0.75rem;border-radius:8px;overflow:auto;}"
    "code{background:#f4f4f4;padding:0.2rem 0.35rem;border-radius:4px;}</style></head>"
    "<body><h1>PX-WiFi-V1</h1><p>Web UI + WebSocket endpoint is running.</p>"
    "<p>Open a websocket to <code>ws://192.168.4.1/ws</code>.</p>"
    "<button id='ping'>Send Ping</button><pre id='log'></pre>"
    "<script>const log=document.getElementById('log');"
    "const ws=new WebSocket('ws://'+location.host+'/ws');"
    "ws.onopen=()=>log.textContent+='[open]\\n';"
    "ws.onmessage=(e)=>log.textContent+='[msg] '+e.data+'\\n';"
    "ws.onclose=()=>log.textContent+='[close]\\n';"
    "document.getElementById('ping').onclick=()=>ws.send('{\"command\":\"ping\"}');"
    "</script></body></html>";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake complete");
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
    };

    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ws recv size failed: %s", esp_err_to_name(ret));
        return ret;
    }

    char payload[128] = {0};
    if (frame.len > 0) {
        size_t copy_len = frame.len < sizeof(payload) - 1 ? frame.len : sizeof(payload) - 1;
        frame.payload = (uint8_t *)payload;
        frame.len = copy_len;

        ret = httpd_ws_recv_frame(req, &frame, copy_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ws recv payload failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "WS RX: %s", payload[0] ? payload : "<empty>");

    const char *reply = "{\"event\":\"pong\",\"data\":{\"source\":\"px-wifi-v1\"}}";
    httpd_ws_frame_t out = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)reply,
        .len = strlen(reply),
    };

    return httpd_ws_send_frame(req, &out);
}

static esp_err_t start_softap(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len = 0,
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "Paradox-PXWiFiV1-%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: SSID=%s", ap_cfg.ap.ssid);
    ESP_LOGI(TAG, "Browse to http://192.168.4.1/");

    return ESP_OK;
}

esp_err_t web_ui_start(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(start_softap());

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL,
    };
    httpd_register_uri_handler(server, &ws_uri);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

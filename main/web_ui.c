#include "web_ui.h"
#include "prop_engine.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mdns.h"
#include "nvs_flash.h"

static const char *TAG = "web_ui";

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    const char *content_type;
    const char *cache_control;
} static_asset_t;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t config_html_start[] asm("_binary_config_html_start");
extern const uint8_t config_html_end[] asm("_binary_config_html_end");
extern const uint8_t connection_html_start[] asm("_binary_connection_html_start");
extern const uint8_t connection_html_end[] asm("_binary_connection_html_end");
extern const uint8_t styles_css_start[] asm("_binary_styles_css_start");
extern const uint8_t styles_css_end[] asm("_binary_styles_css_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");
extern const uint8_t logo_png_start[] asm("_binary_logo_png_start");
extern const uint8_t logo_png_end[] asm("_binary_logo_png_end");

static const static_asset_t ASSET_INDEX = {
    .start = index_html_start,
    .end = index_html_end,
    .content_type = "text/html; charset=utf-8",
    .cache_control = "no-store",
};

static const static_asset_t ASSET_CONFIG = {
    .start = config_html_start,
    .end = config_html_end,
    .content_type = "text/html; charset=utf-8",
    .cache_control = "no-store",
};

static const static_asset_t ASSET_CONNECTION = {
    .start = connection_html_start,
    .end = connection_html_end,
    .content_type = "text/html; charset=utf-8",
    .cache_control = "no-store",
};

static const static_asset_t ASSET_STYLES = {
    .start = styles_css_start,
    .end = styles_css_end,
    .content_type = "text/css; charset=utf-8",
    .cache_control = "public, max-age=600",
};

static const static_asset_t ASSET_APP_JS = {
    .start = app_js_start,
    .end = app_js_end,
    .content_type = "application/javascript; charset=utf-8",
    .cache_control = "public, max-age=600",
};

static const static_asset_t ASSET_LOGO = {
    .start = logo_png_start,
    .end = logo_png_end,
    .content_type = "image/png",
    .cache_control = "public, max-age=3600",
};

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char mqtt_host[128];
    int mqtt_port;
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_base_topic[96];
    char mqtt_commands_topic[128];
    char mqtt_state_topic[128];
    char mqtt_events_topic[128];
    char mqtt_warnings_topic[128];
    char mqtt_game_state_topic[128];
    char mqtt_prop_state_topic[128];
    char network_name[33];
} connection_cfg_t;

static char s_prop_id[32] = "px-wifi-v1";
static bool s_mdns_started;

static connection_cfg_t s_conn_cfg = {
    .wifi_ssid = "",
    .wifi_password = "",
    .mqtt_host = "",
    .mqtt_port = 1883,
    .mqtt_username = "",
    .mqtt_password = "",
    .mqtt_base_topic = "paradox",
    .mqtt_commands_topic = "paradox/site/zone/commands",
    .mqtt_state_topic = "paradox/site/zone/state",
    .mqtt_events_topic = "paradox/site/zone/events",
    .mqtt_warnings_topic = "paradox/site/zone/warnings",
    .mqtt_game_state_topic = "paradox/game/state",
    .mqtt_prop_state_topic = "paradox/state",
    .network_name = "",
};

static void sanitize_network_name(const char *src, char *out, size_t out_size)
{
    size_t w = 0;
    size_t i;

    if (!src || !out || out_size < 2) {
        return;
    }

    for (i = 0; src[i] != '\0' && w < out_size - 1; ++i) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[w++] = c;
        } else if (c == '-' || c == '_') {
            out[w++] = '-';
        }
    }

    if (w == 0) {
        strncpy(out, "px-wifi-v1", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    out[w] = '\0';
}

static void build_default_identity(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);

    if (err == ESP_OK) {
        snprintf(s_prop_id, sizeof(s_prop_id), "px-wifi-v1-%02x%02x", mac[4], mac[5]);
    }

    if (s_conn_cfg.network_name[0] == '\0') {
        snprintf(s_conn_cfg.network_name, sizeof(s_conn_cfg.network_name), "%s", s_prop_id);
    }
}

static esp_err_t apply_mdns_hostname(void)
{
    char host[33] = {0};

    sanitize_network_name(s_conn_cfg.network_name, host, sizeof(host));
    strncpy(s_conn_cfg.network_name, host, sizeof(s_conn_cfg.network_name) - 1);
    s_conn_cfg.network_name[sizeof(s_conn_cfg.network_name) - 1] = '\0';

    if (!s_mdns_started) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_mdns_started = true;
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    mdns_hostname_set(host);
    mdns_instance_name_set(s_prop_id);
    ESP_LOGI(TAG, "mDNS hostname set to %s.local", host);
    return ESP_OK;
}

static esp_err_t read_request_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int received = 0;
    int total = req->content_len;

    if (total <= 0 || total >= (int)buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            return ESP_FAIL;
        }
        received += r;
    }

    buf[received] = '\0';
    return ESP_OK;
}

static bool json_extract_string_local(const char *json, const char *key, char *out, size_t out_size)
{
    char key_pat[48];
    const char *p;
    const char *q;
    size_t len;

    snprintf(key_pat, sizeof(key_pat), "\"%s\"", key);
    p = strstr(json, key_pat);
    if (!p) {
        return false;
    }

    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    q = strchr(p, '"');
    if (!q) {
        return false;
    }

    len = (size_t)(q - p);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool json_extract_int_local(const char *json, const char *key, int *out)
{
    char key_pat[48];
    const char *p;

    snprintf(key_pat, sizeof(key_pat), "\"%s\"", key);
    p = strstr(json, key_pat);
    if (!p) {
        return false;
    }

    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }

    *out = (int)strtol(p, NULL, 10);
    return true;
}

static void copy_bounded_local(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static esp_err_t static_asset_handler(httpd_req_t *req)
{
    const static_asset_t *asset = (const static_asset_t *)req->user_ctx;
    size_t len;

    if (!asset || !asset->start || !asset->end || asset->end < asset->start) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Asset unavailable");
        return ESP_FAIL;
    }

    len = (size_t)(asset->end - asset->start);
    httpd_resp_set_type(req, asset->content_type);
    httpd_resp_set_hdr(req, "Cache-Control", asset->cache_control);
    return httpd_resp_send(req, (const char *)asset->start, (int)len);
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
    char payload[768];
    prop_engine_get_state_json(payload, sizeof(payload));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char payload[1024];
    prop_engine_get_config_json(payload, sizeof(payload));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_defaults_get_handler(httpd_req_t *req)
{
    char payload[1024];
    prop_engine_get_default_config_json(payload, sizeof(payload));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t command_post_handler(httpd_req_t *req)
{
    char body[512];
    char response[768];

    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(prop_engine_handle_command_json(body, response, sizeof(response)));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char body[1024];
    char response[256];
    bool persist = strstr(req->uri, "/save") != NULL;

    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(prop_engine_apply_config_json(body, persist, response, sizeof(response)));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_restore_post_handler(httpd_req_t *req)
{
    char response[256];
    bool persist = strstr(req->uri, "/save") != NULL;

    ESP_ERROR_CHECK(prop_engine_restore_defaults(persist, response, sizeof(response)));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t connection_get_handler(httpd_req_t *req)
{
    char payload[2048];

    snprintf(payload,
             sizeof(payload),
             "{"
             "\"wifiSsid\":\"%s\","
             "\"wifiPassword\":\"%s\","
             "\"mqttHost\":\"%s\","
             "\"mqttPort\":%d,"
             "\"mqttUsername\":\"%s\","
             "\"mqttPassword\":\"%s\","
             "\"mqttBaseTopic\":\"%s\","
             "\"mqttCommandTopic\":\"%s\","
             "\"mqttStateTopic\":\"%s\","
             "\"mqttEventsTopic\":\"%s\","
             "\"mqttWarningsTopic\":\"%s\","
             "\"mqttGameStateTopic\":\"%s\","
             "\"mqttPropStateTopic\":\"%s\","
             "\"networkName\":\"%s\""
             "}",
             s_conn_cfg.wifi_ssid,
             s_conn_cfg.wifi_password,
             s_conn_cfg.mqtt_host,
             s_conn_cfg.mqtt_port,
             s_conn_cfg.mqtt_username,
             s_conn_cfg.mqtt_password,
             s_conn_cfg.mqtt_base_topic,
             s_conn_cfg.mqtt_commands_topic,
             s_conn_cfg.mqtt_state_topic,
             s_conn_cfg.mqtt_events_topic,
             s_conn_cfg.mqtt_warnings_topic,
             s_conn_cfg.mqtt_game_state_topic,
             s_conn_cfg.mqtt_prop_state_topic,
             s_conn_cfg.network_name);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t connection_post_handler(httpd_req_t *req)
{
    char body[1024];
    char value[128];

    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    if (json_extract_string_local(body, "wifiSsid", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.wifi_ssid, sizeof(s_conn_cfg.wifi_ssid), value);
    }
    if (json_extract_string_local(body, "wifiPassword", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.wifi_password, sizeof(s_conn_cfg.wifi_password), value);
    }
    if (json_extract_string_local(body, "mqttHost", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_host, sizeof(s_conn_cfg.mqtt_host), value);
    }
    if (json_extract_int_local(body, "mqttPort", &s_conn_cfg.mqtt_port)) {
        if (s_conn_cfg.mqtt_port < 1 || s_conn_cfg.mqtt_port > 65535) {
            s_conn_cfg.mqtt_port = 1883;
        }
    }
    if (json_extract_string_local(body, "mqttUsername", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_username, sizeof(s_conn_cfg.mqtt_username), value);
    }
    if (json_extract_string_local(body, "mqttPassword", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_password, sizeof(s_conn_cfg.mqtt_password), value);
    }
    if (json_extract_string_local(body, "mqttBaseTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_base_topic, sizeof(s_conn_cfg.mqtt_base_topic), value);
    }
    if (json_extract_string_local(body, "mqttCommandTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_commands_topic, sizeof(s_conn_cfg.mqtt_commands_topic), value);
    }
    if (json_extract_string_local(body, "mqttStateTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_state_topic, sizeof(s_conn_cfg.mqtt_state_topic), value);
    }
    if (json_extract_string_local(body, "mqttEventsTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_events_topic, sizeof(s_conn_cfg.mqtt_events_topic), value);
    }
    if (json_extract_string_local(body, "mqttWarningsTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_warnings_topic, sizeof(s_conn_cfg.mqtt_warnings_topic), value);
    }
    if (json_extract_string_local(body, "mqttGameStateTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_game_state_topic, sizeof(s_conn_cfg.mqtt_game_state_topic), value);
    }
    if (json_extract_string_local(body, "mqttPropStateTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_prop_state_topic, sizeof(s_conn_cfg.mqtt_prop_state_topic), value);
    }
    if (json_extract_string_local(body, "networkName", value, sizeof(value))) {
        sanitize_network_name(value, s_conn_cfg.network_name, sizeof(s_conn_cfg.network_name));
        (void)apply_mdns_hostname();
    }

    ESP_LOGI(TAG, "Connection config updated (wifi ssid='%s', mqtt host='%s:%d')",
             s_conn_cfg.wifi_ssid, s_conn_cfg.mqtt_host, s_conn_cfg.mqtt_port);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"applied\":true}");
}

static esp_err_t device_details_get_handler(httpd_req_t *req)
{
    char state_json[768] = {0};
    char game_state[32] = "unknown";
    int battery = -1;
    int64_t free_heap = (int64_t)esp_get_free_heap_size();
    const esp_app_desc_t *app = esp_app_get_description();
    char ip_text[32] = "192.168.4.1";
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            snprintf(ip_text,
                     sizeof(ip_text),
                     IPSTR,
                     IP2STR(&ip_info.ip));
        }
    }

    prop_engine_get_state_json(state_json, sizeof(state_json));
    (void)json_extract_int_local(state_json, "battery", &battery);
    (void)json_extract_string_local(state_json, "gameState", game_state, sizeof(game_state));

    char payload[1024];
    snprintf(payload,
             sizeof(payload),
             "{"
             "\"propName\":\"%s\","
             "\"ipAddress\":\"%s\","
             "\"softwareVersion\":\"%s\","
             "\"buildNumber\":\"%s\","
             "\"buildDate\":\"%s %s\","
             "\"cpuTempC\":null,"
             "\"freeMemoryBytes\":%lld,"
             "\"batteryPercent\":%d,"
             "\"networkName\":\"%s\","
             "\"status\":\"%s\""
             "}",
             s_prop_id,
             ip_text,
             app->version,
             app->version,
             app->date,
             app->time,
             (long long)free_heap,
             battery,
             s_conn_cfg.network_name,
             game_state);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t device_name_post_handler(httpd_req_t *req)
{
    char body[256];
    char name_in[64];
    char response[256];

    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    if (!json_extract_string_local(body, "networkName", name_in, sizeof(name_in))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing networkName");
        return ESP_FAIL;
    }

    sanitize_network_name(name_in, s_conn_cfg.network_name, sizeof(s_conn_cfg.network_name));
    (void)apply_mdns_hostname();

    snprintf(response,
             sizeof(response),
             "{\"ok\":true,\"networkName\":\"%s\",\"url\":\"http://%s.local\"}",
             s_conn_cfg.network_name,
             s_conn_cfg.network_name);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t connection_scan_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t records[16];
    uint16_t count = 16;
    wifi_scan_config_t scan_cfg = {0};
    char *payload;
    size_t pos = 0;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"scanFailed\",\"networks\":[]}");
    }

    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"scanReadFailed\",\"networks\":[]}");
    }

    payload = (char *)calloc(1, 4096);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    pos += (size_t)snprintf(payload + pos, 4096 - pos, "{\"ok\":true,\"networks\":[");
    for (uint16_t i = 0; i < count; ++i) {
        if (i > 0) {
            pos += (size_t)snprintf(payload + pos, 4096 - pos, ",");
        }
        pos += (size_t)snprintf(payload + pos,
                                4096 - pos,
                                "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                                (const char *)records[i].ssid,
                                (int)records[i].rssi,
                                (int)records[i].authmode);
        if (pos >= 4000) {
            break;
        }
    }
    (void)snprintf(payload + pos, 4096 - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
    free(payload);
    return err;
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

    char reply_buf[768];
    if (payload[0] == '\0') {
        snprintf(reply_buf, sizeof(reply_buf), "{\"ok\":false,\"error\":\"emptyPayload\"}");
    } else {
        ESP_ERROR_CHECK(prop_engine_handle_command_json(payload, reply_buf, sizeof(reply_buf)));
    }

    httpd_ws_frame_t out = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)reply_buf,
        .len = strlen(reply_buf),
    };

    return httpd_ws_send_frame(req, &out);
}

static esp_err_t start_softap(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

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

    build_default_identity();
    (void)apply_mdns_hostname();

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
        .handler = static_asset_handler,
        .user_ctx = (void *)&ASSET_INDEX,
    };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t index_html_uri = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = static_asset_handler,
        .user_ctx = (void *)&ASSET_INDEX,
    };
    httpd_register_uri_handler(server, &index_html_uri);

    httpd_uri_t config_html_uri = {
        .uri = "/config.html",
        .method = HTTP_GET,
        .handler = static_asset_handler,
        .user_ctx = (void *)&ASSET_CONFIG,
    };
    httpd_register_uri_handler(server, &config_html_uri);

    httpd_uri_t connection_html_uri = {
        .uri = "/connection.html",
        .method = HTTP_GET,
        .handler = static_asset_handler,
        .user_ctx = (void *)&ASSET_CONNECTION,
    };
    httpd_register_uri_handler(server, &connection_html_uri);

    httpd_uri_t styles_css_uri = {
        .uri = "/styles.css",
        .method = HTTP_GET,
        .handler = static_asset_handler,
        .user_ctx = (void *)&ASSET_STYLES,
    };
    httpd_register_uri_handler(server, &styles_css_uri);

    httpd_uri_t app_js_uri = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = static_asset_handler,
        .user_ctx = (void *)&ASSET_APP_JS,
    };
    httpd_register_uri_handler(server, &app_js_uri);

    httpd_uri_t logo_png_uri = {
        .uri = "/assets/logo.png",
        .method = HTTP_GET,
        .handler = static_asset_handler,
        .user_ctx = (void *)&ASSET_LOGO,
    };
    httpd_register_uri_handler(server, &logo_png_uri);

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

    httpd_uri_t state_uri = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = state_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &state_uri);

    httpd_uri_t config_get_uri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &config_get_uri);

    httpd_uri_t config_defaults_uri = {
        .uri = "/api/config/defaults",
        .method = HTTP_GET,
        .handler = config_defaults_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &config_defaults_uri);

    httpd_uri_t command_post_uri = {
        .uri = "/api/command",
        .method = HTTP_POST,
        .handler = command_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &command_post_uri);

    httpd_uri_t config_post_uri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &config_post_uri);

    httpd_uri_t config_save_post_uri = {
        .uri = "/api/config/save",
        .method = HTTP_POST,
        .handler = config_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &config_save_post_uri);

    httpd_uri_t config_restore_post_uri = {
        .uri = "/api/config/restore",
        .method = HTTP_POST,
        .handler = config_restore_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &config_restore_post_uri);

    httpd_uri_t config_restore_save_post_uri = {
        .uri = "/api/config/restore/save",
        .method = HTTP_POST,
        .handler = config_restore_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &config_restore_save_post_uri);

    httpd_uri_t connection_get_uri = {
        .uri = "/api/connection",
        .method = HTTP_GET,
        .handler = connection_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &connection_get_uri);

    httpd_uri_t connection_post_uri = {
        .uri = "/api/connection",
        .method = HTTP_POST,
        .handler = connection_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &connection_post_uri);

    httpd_uri_t connection_scan_uri = {
        .uri = "/api/connection/scan",
        .method = HTTP_GET,
        .handler = connection_scan_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &connection_scan_uri);

    httpd_uri_t device_details_uri = {
        .uri = "/api/device/details",
        .method = HTTP_GET,
        .handler = device_details_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &device_details_uri);

    httpd_uri_t device_name_uri = {
        .uri = "/api/device/name",
        .method = HTTP_POST,
        .handler = device_name_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &device_name_uri);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

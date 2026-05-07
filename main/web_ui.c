#include "web_ui.h"
#include "web_ui_json.h"
#include "prop_engine.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mqtt_client.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_ui";

static esp_err_t wifi_connect_sta(const char *ssid, const char *password);
static bool json_extract_string_local(const char *json, const char *key, char *out, size_t out_size);
static bool json_extract_int_local(const char *json, const char *key, int *out);
static bool json_extract_bool_local(const char *json, const char *key, bool *out);
static void json_escape_string_local(const char *src, char *dst, size_t dst_size);
static void copy_bounded_local(char *dst, size_t dst_size, const char *src);
static esp_err_t save_connection_cfg_nvs(void);
static void mqtt_start_client(void);
static void mqtt_stop_client(void);
static void ota_reboot_task(void *arg);

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
extern const uint8_t update_html_start[] asm("_binary_update_html_start");
extern const uint8_t update_html_end[] asm("_binary_update_html_end");
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
    .cache_control = "no-store",
};

static const static_asset_t ASSET_APP_JS = {
    .start = app_js_start,
    .end = app_js_end,
    .content_type = "application/javascript; charset=utf-8",
    .cache_control = "no-store",
};

static const static_asset_t ASSET_UPDATE = {
    .start = update_html_start,
    .end = update_html_end,
    .content_type = "text/html; charset=utf-8",
    .cache_control = "no-store",
};

static const static_asset_t ASSET_LOGO = {
    .start = logo_png_start,
    .end = logo_png_end,
    .content_type = "image/png",
    .cache_control = "public, max-age=3600",
};

static char s_prop_id[32] = "px-wifi-v1";
static bool s_mdns_started;
static bool s_sta_connecting;
static int s_sta_retry_count;
static esp_timer_handle_t s_sta_reconnect_timer;
static esp_timer_handle_t s_ap_shutdown_timer;
static bool s_ap_shutdown_pending;
static esp_mqtt_client_handle_t s_mqtt_client;
static bool s_mqtt_connected;
static char s_mqtt_uri[196] = "";
static char s_sta_ip_text[32] = "";
static int s_sta_last_disconnect_reason;
static char s_sta_last_error[48] = "";

static connection_cfg_t s_conn_cfg = WEB_UI_CONNECTION_CFG_DEFAULT;

static esp_err_t load_connection_cfg_nvs(void)
{
    return web_ui_json_load_connection_cfg(WEB_UI_CONFIG_FILE_PATH, &s_conn_cfg);
}

static esp_err_t save_connection_cfg_nvs(void)
{
    char prop_cfg[4096];

    prop_engine_get_config_json(prop_cfg, sizeof(prop_cfg));
    return web_ui_json_save_connection_cfg(WEB_UI_CONFIG_FILE_PATH, &s_conn_cfg, prop_cfg);
}

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
    esp_netif_t *ap_netif;
    esp_netif_t *sta_netif;

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

    ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (ap_netif) {
        (void)esp_netif_set_hostname(ap_netif, host);
    }
    if (sta_netif) {
        (void)esp_netif_set_hostname(sta_netif, host);
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
    return web_ui_json_extract_string(json, key, out, out_size);
}

static bool json_extract_int_local(const char *json, const char *key, int *out)
{
    return web_ui_json_extract_int(json, key, out);
}

static bool json_extract_bool_local(const char *json, const char *key, bool *out)
{
    return web_ui_json_extract_bool(json, key, out);
}

static void json_escape_string_local(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789abcdef";
    size_t w = 0;

    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    for (size_t i = 0; src[i] != '\0' && w + 1 < dst_size; ++i) {
        unsigned char c = (unsigned char)src[i];
        const char *escape = NULL;

        switch (c) {
            case '"':
                escape = "\\\"";
                break;
            case '\\':
                escape = "\\\\";
                break;
            case '\b':
                escape = "\\b";
                break;
            case '\f':
                escape = "\\f";
                break;
            case '\n':
                escape = "\\n";
                break;
            case '\r':
                escape = "\\r";
                break;
            case '\t':
                escape = "\\t";
                break;
            default:
                break;
        }

        if (escape) {
            size_t escape_len = strlen(escape);
            if (w + escape_len >= dst_size) {
                break;
            }
            memcpy(dst + w, escape, escape_len);
            w += escape_len;
            continue;
        }

        if (c < 0x20) {
            if (w + 6 >= dst_size) {
                break;
            }
            dst[w++] = '\\';
            dst[w++] = 'u';
            dst[w++] = '0';
            dst[w++] = '0';
            dst[w++] = hex[(c >> 4) & 0x0F];
            dst[w++] = hex[c & 0x0F];
            continue;
        }

        dst[w++] = (char)c;
    }

    dst[w] = '\0';
}

static void copy_bounded_local(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static const char *mqtt_base_topic_or_default(void)
{
    return s_conn_cfg.mqtt_base_topic[0] != '\0' ? s_conn_cfg.mqtt_base_topic : "site/room/zone";
}

static const char *prop_led_hint_name_local(prop_led_hint_t hint);

static void mqtt_build_topic(char *out, size_t out_size, const char *suffix)
{
    snprintf(out, out_size, "%s/%s", mqtt_base_topic_or_default(), suffix);
}

static int mqtt_state_interval_ms(void)
{
    char cfg[1024];
    int heartbeat = 10000;

    prop_engine_get_config_json(cfg, sizeof(cfg));
    if (json_extract_int_local(cfg, "heartbeatInterval", &heartbeat)) {
        if (heartbeat < 1000) {
            heartbeat = 1000;
        }
        if (heartbeat > 120000) {
            heartbeat = 120000;
        }
    }
    return heartbeat;
}

static void mqtt_publish_state(bool retained)
{
    char state_json[1024];
    char topic[160];

    if (!s_mqtt_client || !s_mqtt_connected) {
        return;
    }

    mqtt_build_topic(topic, sizeof(topic), "state");
    prop_engine_get_state_json(state_json, sizeof(state_json));
    esp_mqtt_client_publish(s_mqtt_client,
                            topic,
                            state_json,
                            0,
                            1,
                            retained ? 1 : 0);
}

static bool mqtt_publish_engine_events(bool publish_state_after)
{
    char event_json[384];
    char topic[160];
    bool had_events = false;

    if (!s_mqtt_client || !s_mqtt_connected) {
        return false;
    }

    mqtt_build_topic(topic, sizeof(topic), "events");
    while (prop_engine_pop_event_json(event_json, sizeof(event_json))) {
        esp_mqtt_client_publish(s_mqtt_client, topic, event_json, 0, 1, 0);
        had_events = true;
    }

    if (had_events && publish_state_after) {
        mqtt_publish_state(true);
    }

    return had_events;
}

static void mqtt_publish_announce(void)
{
    char announce[1024];
    char prop_cfg[2048];
    char state_topic[160];
    char commands_topic[160];
    char host[33] = {0};
    char ip_text[32] = "unavailable";
    char battery_profile[32] = "unknown";
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    const esp_app_desc_t *app = esp_app_get_description();
    int wire_count = 0;
    int battery_adc_raw = 0;
    int battery_adc_at_0v = 0;
    int battery_adc_at_15v = 0;

    if (!s_mqtt_client || !s_mqtt_connected || s_conn_cfg.mqtt_prop_announce_topic[0] == '\0') {
        return;
    }

    if (sta_netif) {
        esp_netif_ip_info_t sta_ip;
        if (esp_netif_get_ip_info(sta_netif, &sta_ip) == ESP_OK) {
            snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&sta_ip.ip));
        }
    }

    mqtt_build_topic(state_topic, sizeof(state_topic), "state");
    mqtt_build_topic(commands_topic, sizeof(commands_topic), "commands");
    prop_engine_get_config_json(prop_cfg, sizeof(prop_cfg));
    (void)json_extract_int_local(prop_cfg, "wireCount", &wire_count);
    (void)json_extract_int_local(prop_cfg, "batteryAdcRaw", &battery_adc_raw);
    (void)json_extract_int_local(prop_cfg, "batteryAdcAt0V", &battery_adc_at_0v);
    (void)json_extract_int_local(prop_cfg, "batteryAdcAt15V", &battery_adc_at_15v);
    (void)json_extract_string_local(prop_cfg,
                                    "batteryProfile",
                                    battery_profile,
                                    sizeof(battery_profile));

    sanitize_network_name(s_conn_cfg.network_name, host, sizeof(host));
    snprintf(announce,
             sizeof(announce),
             "{"
             "\"ts\":%lld,"
             "\"event\":\"online\","
             "\"propId\":\"%s\","
             "\"propName\":\"%s\","
             "\"ip\":\"%s\","
             "\"mdns\":\"%s.local\","
             "\"wireCount\":%d,"
             "\"batteryAdcRaw\":%d,"
             "\"batteryAdcAt0V\":%d,"
             "\"batteryAdcAt15V\":%d,"
             "\"batteryProfile\":\"%s\","
             "\"ledHint\":\"%s\","
             "\"version\":\"%s\","
             "\"buildId\":\"%s\","
             "\"buildDate\":\"%s\","
             "\"buildTime\":\"%s\","
             "\"stateTopic\":\"%s\","
             "\"commandsTopic\":\"%s\""
             "}",
             (long long)(esp_timer_get_time() / 1000),
             s_prop_id,
             s_prop_id,
             ip_text,
             host,
             wire_count,
             battery_adc_raw,
             battery_adc_at_0v,
             battery_adc_at_15v,
             battery_profile,
             prop_led_hint_name_local(prop_engine_get_led_hint()),
             app->version,
             app->version,
             app->date,
             app->time,
             state_topic,
             commands_topic);

    esp_mqtt_client_publish(s_mqtt_client,
                            s_conn_cfg.mqtt_prop_announce_topic,
                            announce,
                            0,
                            1,
                            0);
}

static void mqtt_publish_warning(const char *message)
{
    char payload[384];
    char topic[160];
    char warning_json[193];

    if (!s_mqtt_client || !s_mqtt_connected) {
        return;
    }

    mqtt_build_topic(topic, sizeof(topic), "warnings");
    json_escape_string_local(message ? message : "unknown", warning_json, sizeof(warning_json));

    snprintf(payload,
             sizeof(payload),
             "{\"ts\":%lld,\"warning\":\"%s\"}",
             (long long)(esp_timer_get_time() / 1000),
             warning_json);
    esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
}

static bool mqtt_get_follow_cfg(bool *enabled, int *tolerance_ms)
{
    char cfg[1024];
    bool keep_sync = false;
    int tol = 1000;

    prop_engine_get_config_json(cfg, sizeof(cfg));
    if (!json_extract_bool_local(cfg, "keepSyncEnabled", &keep_sync)) {
        keep_sync = false;
    }
    if (json_extract_int_local(cfg, "timeToleranceMs", &tol)) {
        if (tol < 0) {
            tol = 0;
        }
        if (tol > 10000) {
            tol = 10000;
        }
    }

    if (enabled) {
        *enabled = keep_sync;
    }
    if (tolerance_ms) {
        *tolerance_ms = tol;
    }
    return true;
}

static int parse_time_string_to_seconds(const char *text)
{
    int mm;
    int ss;

    if (!text || text[0] == '\0') {
        return -1;
    }

    if (sscanf(text, "%d:%d", &mm, &ss) == 2) {
        if (mm < 0 || ss < 0) {
            return -1;
        }
        return (mm * 60) + ss;
    }

    return (int)strtol(text, NULL, 10);
}

static const char *prop_led_hint_name_local(prop_led_hint_t hint)
{
    switch (hint) {
        case PROP_LED_HINT_AP_MODE:
            return "ap";
        case PROP_LED_HINT_CONNECTING_WIFI:
            return "wifi_connecting";
        case PROP_LED_HINT_CONNECTING_MQTT:
            return "mqtt_connecting";
        case PROP_LED_HINT_READY:
            return "ready";
        case PROP_LED_HINT_NOT_READY:
            return "not_ready";
        case PROP_LED_HINT_COUNTDOWN:
            return "countdown";
        case PROP_LED_HINT_PAUSED:
            return "paused";
        case PROP_LED_HINT_PENALTY:
            return "penalty";
        case PROP_LED_HINT_DETONATED:
            return "detonated";
        case PROP_LED_HINT_DEFUSED:
            return "defused";
        case PROP_LED_HINT_OTA:
            return "ota";
        case PROP_LED_HINT_OFF:
        default:
            return "off";
    }
}

static void mqtt_publish_follow_event(const char *event,
                                      int before_s,
                                      int after_s,
                                      const char *message)
{
    char payload[384];
    char topic[160];

    if (!s_mqtt_client || !s_mqtt_connected) {
        return;
    }

    mqtt_build_topic(topic, sizeof(topic), "events");

    snprintf(payload,
             sizeof(payload),
             "{"
             "\"ts\":%lld,"
             "\"event\":\"%s\","
             "\"before\":%d,"
             "\"after\":%d,"
             "\"message\":\"%s\""
             "}",
             (long long)(esp_timer_get_time() / 1000),
             event ? event : "followEvent",
             before_s,
             after_s,
             message ? message : "");
    esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
}

static void mqtt_apply_follower_payload(const char *payload, int payload_len)
{
    char msg[768];
    char local_state_json[1024];
    char local_state[32] = "";
    char game_mode[32] = "";
    char game_state[32] = "";
    char remaining_text[16] = "";
    char cmd[160];
    char response[512];
    bool enabled = false;
    bool game_paused = false;
    bool has_pause = false;
    int tolerance_ms = 1000;
    int local_remaining_s = -1;
    int remote_remaining_s = -1;
    cJSON *remote_root = NULL;
    cJSON *local_root = NULL;

    if (!payload || payload_len <= 0) {
        return;
    }

    mqtt_get_follow_cfg(&enabled, &tolerance_ms);
    if (!enabled) {
        return;
    }

    if (payload_len >= (int)sizeof(msg)) {
        payload_len = (int)sizeof(msg) - 1;
    }
    memcpy(msg, payload, (size_t)payload_len);
    msg[payload_len] = '\0';

    remote_root = web_ui_json_parse(msg);
    if (!remote_root) {
        return;
    }

    if (!web_ui_json_get_int(remote_root, "timeRemaining", &remote_remaining_s)) {
        if (web_ui_json_get_string(remote_root, "remaining_time", remaining_text, sizeof(remaining_text))) {
            remote_remaining_s = parse_time_string_to_seconds(remaining_text);
        }
    }
    if (remote_remaining_s < 0) {
        cJSON_Delete(remote_root);
        return;
    }

    (void)web_ui_json_get_bool(remote_root, "gamePaused", &game_paused);
    has_pause = web_ui_json_has_key(remote_root, "gamePaused");
    (void)web_ui_json_get_string(remote_root, "gameMode", game_mode, sizeof(game_mode));
    (void)web_ui_json_get_string(remote_root, "state", game_state, sizeof(game_state));

    prop_engine_get_state_json(local_state_json, sizeof(local_state_json));
    local_root = web_ui_json_parse(local_state_json);
    if (local_root) {
        (void)web_ui_json_get_int(local_root, "timeRemaining", &local_remaining_s);
        (void)web_ui_json_get_string(local_root, "gameState", local_state, sizeof(local_state));
    }

    if (local_remaining_s >= 0 &&
        abs((local_remaining_s - remote_remaining_s) * 1000) > tolerance_ms) {
        snprintf(cmd, sizeof(cmd), "{\"command\":\"setTime\",\"time\":%d}", remote_remaining_s);
        if (prop_engine_handle_command_json(cmd, response, sizeof(response)) == ESP_OK) {
            mqtt_publish_follow_event("syncAdjusted",
                                      local_remaining_s,
                                      remote_remaining_s,
                                      "Follower adjusted local timer");
        }
    }

    if (has_pause && game_paused) {
        if (strcmp(local_state, "countdown") == 0) {
            if (prop_engine_handle_command_json("{\"command\":\"pause\"}", response, sizeof(response)) == ESP_OK) {
                mqtt_publish_follow_event("commandOverridden",
                                          local_remaining_s,
                                          remote_remaining_s,
                                          "Follower paused to match game state");
            }
        }
    } else if ((strcmp(game_mode, "running") == 0 || strcmp(game_state, "running") == 0 ||
                strcmp(game_state, "countdown") == 0) &&
               (strcmp(local_state, "paused") == 0 || strcmp(local_state, "ready") == 0)) {
        if (prop_engine_handle_command_json("{\"command\":\"resume\"}", response, sizeof(response)) == ESP_OK) {
            mqtt_publish_follow_event("commandOverridden",
                                      local_remaining_s,
                                      remote_remaining_s,
                                      "Follower resumed to match game state");
        }
    }

    mqtt_publish_state(true);
    cJSON_Delete(local_root);
    cJSON_Delete(remote_root);
}

static void mqtt_handle_command_payload(const char *payload, int payload_len)
{
    char cmd_json[512];
    char response[1024];
    char command_name[32] = "";
    char warning_message[96] = "";
    bool ok = true;
    bool has_error_message = false;
    bool has_warning_message = false;
    char events_topic[160];
    int copy_len;
    cJSON *cmd_root = NULL;
    cJSON *response_root = NULL;

    if (!payload || payload_len <= 0) {
        return;
    }

    copy_len = payload_len;
    if (copy_len >= (int)sizeof(cmd_json)) {
        copy_len = (int)sizeof(cmd_json) - 1;
    }
    memcpy(cmd_json, payload, (size_t)copy_len);
    cmd_json[copy_len] = '\0';
    cmd_root = web_ui_json_parse(cmd_json);
    if (cmd_root) {
        (void)web_ui_json_get_string(cmd_root, "command", command_name, sizeof(command_name));
    }

    if (prop_engine_handle_command_json(cmd_json, response, sizeof(response)) == ESP_OK) {
        mqtt_build_topic(events_topic, sizeof(events_topic), "events");
        esp_mqtt_client_publish(s_mqtt_client, events_topic, response, 0, 1, 0);
        response_root = web_ui_json_parse(response);
        has_warning_message = web_ui_json_get_string(response_root, "message", warning_message, sizeof(warning_message));
        if (!has_warning_message) {
            has_error_message = web_ui_json_get_string(response_root, "error", warning_message, sizeof(warning_message));
        }
        if ((web_ui_json_get_bool(response_root, "ok", &ok) && !ok) || has_error_message || has_warning_message) {
            if (!warning_message[0]) {
                snprintf(warning_message, sizeof(warning_message), "command_rejected");
            }
            mqtt_publish_warning(warning_message);
        }
        if (!mqtt_publish_engine_events(true)) {
            mqtt_publish_state(true);
        }
        if (strcmp(command_name, "reboot") == 0 && (!web_ui_json_get_bool(response_root, "ok", &ok) || ok)) {
            xTaskCreate(ota_reboot_task, "cmd_reboot", 2048, NULL, 5, NULL);
        }
        cJSON_Delete(response_root);
        cJSON_Delete(cmd_root);
        return;
    }

    cJSON_Delete(response_root);
    cJSON_Delete(cmd_root);
    mqtt_publish_warning("command_handle_failed");
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            {
                bool follow_enabled = false;
                int unused_tol = 0;
                char commands_topic[160];

            s_mqtt_connected = true;
            ESP_LOGI(TAG, "MQTT connected");
                mqtt_build_topic(commands_topic, sizeof(commands_topic), "commands");
                esp_mqtt_client_subscribe(s_mqtt_client, commands_topic, 1);
                mqtt_get_follow_cfg(&follow_enabled, &unused_tol);
                if (follow_enabled && s_conn_cfg.mqtt_game_state_topic[0] != '\0') {
                    esp_mqtt_client_subscribe(s_mqtt_client, s_conn_cfg.mqtt_game_state_topic, 1);
                }
            mqtt_publish_announce();
            mqtt_publish_state(true);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_DATA: {
            if (event->topic && event->data) {
                char commands_topic[160];
                mqtt_build_topic(commands_topic, sizeof(commands_topic), "commands");
                if (event->topic_len == (int)strlen(commands_topic) &&
                    strncmp(event->topic, commands_topic, (size_t)event->topic_len) == 0) {
                    mqtt_handle_command_payload(event->data, event->data_len);
                } else if (s_conn_cfg.mqtt_game_state_topic[0] != '\0' &&
                           event->topic_len == (int)strlen(s_conn_cfg.mqtt_game_state_topic) &&
                           strncmp(event->topic, s_conn_cfg.mqtt_game_state_topic, (size_t)event->topic_len) == 0) {
                    mqtt_apply_follower_payload(event->data, event->data_len);
                }
            }
            break;
        }
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "MQTT error event");
            break;
        default:
            break;
    }
}

static void mqtt_stop_client(void)
{
    if (!s_mqtt_client) {
        return;
    }

    esp_mqtt_client_stop(s_mqtt_client);
    esp_mqtt_client_destroy(s_mqtt_client);
    s_mqtt_client = NULL;
    s_mqtt_connected = false;
}

static void mqtt_start_client(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {0};

    mqtt_stop_client();

    if (s_conn_cfg.mqtt_host[0] == '\0') {
        ESP_LOGI(TAG, "MQTT host empty; MQTT client not started");
        return;
    }

    snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "mqtt://%s:%d", s_conn_cfg.mqtt_host, s_conn_cfg.mqtt_port);
    mqtt_cfg.broker.address.uri = s_mqtt_uri;
    mqtt_cfg.credentials.client_id = s_prop_id;
    mqtt_cfg.session.keepalive = 60;
    mqtt_cfg.session.disable_clean_session = true;
    mqtt_cfg.network.reconnect_timeout_ms = 5000;
    if (s_conn_cfg.mqtt_username[0] != '\0') {
        mqtt_cfg.credentials.username = s_conn_cfg.mqtt_username;
    }
    if (s_conn_cfg.mqtt_password[0] != '\0') {
        mqtt_cfg.credentials.authentication.password = s_conn_cfg.mqtt_password;
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return;
    }

    esp_mqtt_client_register_event(s_mqtt_client,
                                   MQTT_EVENT_ANY,
                                   mqtt_event_handler,
                                   NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

static void mqtt_state_task(void *arg)
{
    int64_t last_state_pub_ms = 0;
    bool was_connected = false;

    (void)arg;

    while (true) {
        int interval_ms = mqtt_state_interval_ms();
        int64_t now = esp_timer_get_time() / 1000;

        (void)mqtt_publish_engine_events(true);

        if (!s_mqtt_connected) {
            was_connected = false;
        } else if (!was_connected) {
            last_state_pub_ms = now;
            was_connected = true;
        } else if ((now - last_state_pub_ms) >= interval_ms) {
            mqtt_publish_state(true);
            last_state_pub_ms = now;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static const char *wifi_authmode_to_str(wifi_auth_mode_t authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN:
            return "open";
        case WIFI_AUTH_WEP:
            return "wep";
        case WIFI_AUTH_WPA_PSK:
            return "wpa-psk";
        case WIFI_AUTH_WPA2_PSK:
            return "wpa2-psk";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "wpa-wpa2-psk";
#ifdef WIFI_AUTH_WPA2_ENTERPRISE
        case WIFI_AUTH_WPA2_ENTERPRISE:
            return "wpa2-enterprise";
#endif
#ifdef WIFI_AUTH_WPA3_PSK
        case WIFI_AUTH_WPA3_PSK:
            return "wpa3-psk";
#endif
#ifdef WIFI_AUTH_WPA2_WPA3_PSK
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "wpa2-wpa3-psk";
#endif
#ifdef WIFI_AUTH_WAPI_PSK
        case WIFI_AUTH_WAPI_PSK:
            return "wapi-psk";
#endif
#ifdef WIFI_AUTH_OWE
        case WIFI_AUTH_OWE:
            return "owe";
#endif
        default:
            return "unknown";
    }
}

static const char *wifi_disconnect_reason_to_str(int reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:
            return "auth-expired";
        case WIFI_REASON_AUTH_LEAVE:
            return "auth-leave";
        case WIFI_REASON_ASSOC_TOOMANY:
            return "assoc-too-many";
        case WIFI_REASON_ASSOC_LEAVE:
            return "assoc-leave";
        case WIFI_REASON_ASSOC_NOT_AUTHED:
            return "assoc-not-authed";
        case WIFI_REASON_DISASSOC_PWRCAP_BAD:
            return "disassoc-power-cap-bad";
        case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
            return "disassoc-channel-bad";
        case WIFI_REASON_IE_INVALID:
            return "ie-invalid";
        case WIFI_REASON_MIC_FAILURE:
            return "mic-failure";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            return "4way-timeout";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
            return "group-key-timeout";
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:
            return "ie-4way-differs";
        case WIFI_REASON_GROUP_CIPHER_INVALID:
            return "group-cipher-invalid";
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
            return "pairwise-cipher-invalid";
        case WIFI_REASON_AKMP_INVALID:
            return "akmp-invalid";
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
            return "rsn-version-unsupported";
        case WIFI_REASON_INVALID_RSN_IE_CAP:
            return "rsn-cap-invalid";
        case WIFI_REASON_802_1X_AUTH_FAILED:
            return "8021x-auth-failed";
        case WIFI_REASON_CIPHER_SUITE_REJECTED:
            return "cipher-suite-rejected";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "beacon-timeout";
        case WIFI_REASON_NO_AP_FOUND:
            return "no-ap-found";
        case WIFI_REASON_AUTH_FAIL:
            return "auth-failed";
        case WIFI_REASON_ASSOC_FAIL:
            return "assoc-failed";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "handshake-timeout";
#ifdef WIFI_REASON_CONNECTION_FAIL
        case WIFI_REASON_CONNECTION_FAIL:
            return "connection-failed";
#endif
#ifdef WIFI_REASON_AP_TSF_RESET
        case WIFI_REASON_AP_TSF_RESET:
            return "ap-tsf-reset";
#endif
        default:
            return "unknown";
    }
}

static bool wifi_authmode_is_enterprise(wifi_auth_mode_t authmode)
{
    switch (authmode) {
#ifdef WIFI_AUTH_WPA2_ENTERPRISE
        case WIFI_AUTH_WPA2_ENTERPRISE:
            return true;
#endif
#ifdef WIFI_AUTH_WPA3_ENTERPRISE
        case WIFI_AUTH_WPA3_ENTERPRISE:
            return true;
#endif
#ifdef WIFI_AUTH_WPA2_WPA3_ENTERPRISE
        case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
            return true;
#endif
#ifdef WIFI_AUTH_WPA3_ENT_192
        case WIFI_AUTH_WPA3_ENT_192:
            return true;
#endif
        default:
            return false;
    }
}

static bool wifi_authmode_is_passwordless(wifi_auth_mode_t authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN:
            return true;
#ifdef WIFI_AUTH_OWE
        case WIFI_AUTH_OWE:
            return true;
#endif
        default:
            return false;
    }
}

static bool wifi_lookup_ap_record(const char *ssid, wifi_ap_record_t *out)
{
    wifi_scan_config_t scan_cfg = {0};
    wifi_ap_record_t *records = NULL;
    uint16_t count = 0;
    bool found = false;
    int best_rssi = -127;

    if (!ssid || ssid[0] == '\0' || !out) {
        return false;
    }

    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        return false;
    }
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || count == 0) {
        return false;
    }

    records = (wifi_ap_record_t *)calloc(count, sizeof(*records));
    if (!records) {
        return false;
    }

    if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
        for (uint16_t i = 0; i < count; ++i) {
            if (strcmp((const char *)records[i].ssid, ssid) != 0) {
                continue;
            }
            if (!found || records[i].rssi > best_rssi) {
                *out = records[i];
                best_rssi = records[i].rssi;
                found = true;
            }
        }
    }

    free(records);
    return found;
}

static bool validate_wifi_credentials(const char *ssid, const char *password,
                                      const wifi_ap_record_t *ap_info,
                                      char *err_buf, size_t err_buf_size)
{
    size_t pass_len = password ? strlen(password) : 0;

    if (!ssid || ssid[0] == '\0') {
        return true;
    }

    if (pass_len > 64) {
        snprintf(err_buf, err_buf_size, "WiFi password exceeds the supported 64-character limit.");
        return false;
    }

    if (!ap_info) {
        return true;
    }

    if (wifi_authmode_is_enterprise(ap_info->authmode)) {
        snprintf(err_buf,
                 err_buf_size,
                 "SSID '%s' uses %s, which this UI does not support.",
                 ssid,
                 wifi_authmode_to_str(ap_info->authmode));
        return false;
    }

    if (wifi_authmode_is_passwordless(ap_info->authmode)) {
        if (pass_len > 0) {
            snprintf(err_buf,
                     err_buf_size,
                     "SSID '%s' does not use a WiFi password.",
                     ssid);
            return false;
        }
        return true;
    }

    if (pass_len == 0) {
        snprintf(err_buf,
                 err_buf_size,
                 "SSID '%s' requires a WiFi password.",
                 ssid);
        return false;
    }

    if (ap_info->authmode != WIFI_AUTH_WEP && pass_len < 8) {
        snprintf(err_buf,
                 err_buf_size,
                 "SSID '%s' requires an 8-64 character WiFi password.",
                 ssid);
        return false;
    }

    return true;
}

static wifi_auth_mode_t wifi_select_sta_authmode(const wifi_ap_record_t *ap_info)
{
    if (!ap_info) {
        return WIFI_AUTH_OPEN;
    }

    switch (ap_info->authmode) {
#ifdef WIFI_AUTH_WPA2_WPA3_PSK
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return WIFI_AUTH_WPA2_WPA3_PSK;
#endif
#ifdef WIFI_AUTH_WPA3_PSK
        case WIFI_AUTH_WPA3_PSK:
            return WIFI_AUTH_WPA3_PSK;
#endif
#ifdef WIFI_AUTH_OWE
        case WIFI_AUTH_OWE:
            return WIFI_AUTH_OWE;
#endif
        default:
            return ap_info->authmode;
    }
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
    /* EMBED_TXTFILES appends a \0; strip it so browsers don't see it */
    if (len > 0 && asset->start[len - 1] == '\0') {
        len--;
    }
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

static void build_unified_config_json(char *out, size_t out_size)
{
    char prop_payload[4096];
    char *payload = NULL;
    char commands_topic[160];
    char state_topic[160];
    char events_topic[160];
    char warnings_topic[160];
    prop_engine_get_config_json(prop_payload, sizeof(prop_payload));

    mqtt_build_topic(commands_topic, sizeof(commands_topic), "commands");
    mqtt_build_topic(state_topic, sizeof(state_topic), "state");
    mqtt_build_topic(events_topic, sizeof(events_topic), "events");
    mqtt_build_topic(warnings_topic, sizeof(warnings_topic), "warnings");

    payload = web_ui_json_build_unified_config_payload(&s_conn_cfg,
                                                       commands_topic,
                                                       state_topic,
                                                       events_topic,
                                                       warnings_topic,
                                                       prop_payload);
    if (!payload) {
        snprintf(out, out_size, "{}");
        return;
    }

    copy_bounded_local(out, out_size, payload);
    cJSON_free(payload);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char *payload = (char *)calloc(1, 6144);
    esp_err_t err;

    if (!payload) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    build_unified_config_json(payload, 6144);
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
    free(payload);
    return err;
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
    char command_name[32] = "";
    bool ok = true;

    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    (void)json_extract_string_local(body, "command", command_name, sizeof(command_name));
    ESP_ERROR_CHECK(prop_engine_handle_command_json(body, response, sizeof(response)));
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    if (send_err == ESP_OK && !mqtt_publish_engine_events(true)) {
        mqtt_publish_state(true);
    }
    if (send_err == ESP_OK && strcmp(command_name, "reboot") == 0 &&
        (!json_extract_bool_local(response, "ok", &ok) || ok)) {
        xTaskCreate(ota_reboot_task, "cmd_reboot", 2048, NULL, 5, NULL);
    }
    return send_err;
}

static void ota_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t ota_upload_post_handler(httpd_req_t *req)
{
    const esp_partition_t *update_partition;
    esp_ota_handle_t ota_handle = 0;
    esp_err_t err;
    char buf[1024];
    int remaining;

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing firmware payload");
        return ESP_FAIL;
    }

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return err;
    }

    remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA upload interrupted");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, (const void *)buf, (size_t)received);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return err;
        }

        remaining -= received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA finalize failed");
        return err;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA boot partition failed");
        return err;
    }

    httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"OTA complete, rebooting\"}");
    xTaskCreate(ota_reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    char response[256];
    char value[128];
    bool persist = strstr(req->uri, "/save") != NULL;
    esp_err_t status = ESP_OK;
    cJSON *root = NULL;

    body = (char *)calloc(1, (size_t)req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    if (read_request_body(req, body, (size_t)req->content_len + 1) != ESP_OK) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    root = web_ui_json_parse(body);
    if (!root) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(prop_engine_apply_config_json(body, false, response, sizeof(response)));

    if (web_ui_json_get_string(root, "wifiSsid", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.wifi_ssid, sizeof(s_conn_cfg.wifi_ssid), value);
    }
    if (web_ui_json_get_string(root, "wifiPassword", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.wifi_password, sizeof(s_conn_cfg.wifi_password), value);
    }
    if (web_ui_json_get_string(root, "mqttHost", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_host, sizeof(s_conn_cfg.mqtt_host), value);
    }
    if (web_ui_json_get_int(root, "mqttPort", &s_conn_cfg.mqtt_port)) {
        if (s_conn_cfg.mqtt_port < 1 || s_conn_cfg.mqtt_port > 65535) {
            s_conn_cfg.mqtt_port = 1883;
        }
    }
    if (web_ui_json_get_string(root, "mqttUsername", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_username, sizeof(s_conn_cfg.mqtt_username), value);
    }
    if (web_ui_json_get_string(root, "mqttPassword", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_password, sizeof(s_conn_cfg.mqtt_password), value);
    }
    if (web_ui_json_get_string(root, "mqttBaseTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_base_topic, sizeof(s_conn_cfg.mqtt_base_topic), value);
    }
    if (web_ui_json_get_string(root, "mqttGameStateTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_game_state_topic, sizeof(s_conn_cfg.mqtt_game_state_topic), value);
    }
    if (web_ui_json_get_string(root, "mqttPropAnnounceTopic", value, sizeof(value)) ||
        web_ui_json_get_string(root, "mqttPropStateTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_prop_announce_topic,
                           sizeof(s_conn_cfg.mqtt_prop_announce_topic),
                           value);
    }
    if (web_ui_json_get_string(root, "networkName", value, sizeof(value))) {
        sanitize_network_name(value, s_conn_cfg.network_name, sizeof(s_conn_cfg.network_name));
        (void)apply_mdns_hostname();
    }
    if (web_ui_json_get_string(root, "apPassword", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.ap_password, sizeof(s_conn_cfg.ap_password), value);
    }
    {
        bool ap_val;
        if (web_ui_json_get_bool(root, "apEnabled", &ap_val)) {
            s_conn_cfg.ap_enabled = ap_val;
        }
    }

    if (persist) {
        esp_err_t save_err = save_connection_cfg_nvs();
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to persist unified config: %s", esp_err_to_name(save_err));
        }
    }

    mqtt_start_client();

    httpd_resp_set_type(req, "application/json");
    status = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    cJSON_Delete(root);
    free(body);
    return status;
}

static esp_err_t config_restore_post_handler(httpd_req_t *req)
{
    char response[256];
    bool persist = strstr(req->uri, "/save") != NULL;

    ESP_ERROR_CHECK(prop_engine_restore_defaults(false, response, sizeof(response)));
    if (persist) {
        esp_err_t save_err = save_connection_cfg_nvs();
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to persist unified defaults: %s", esp_err_to_name(save_err));
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t connection_get_handler(httpd_req_t *req)
{
    char *payload;
    char commands_topic[160];
    char state_topic[160];
    char events_topic[160];
    char warnings_topic[160];
    char ap_ip_text[32] = "192.168.4.1";
    char ap_ssid[33] = "Paradox-PXWiFiV1";
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    wifi_config_t ap_cfg = {0};

    mqtt_build_topic(commands_topic, sizeof(commands_topic), "commands");
    mqtt_build_topic(state_topic, sizeof(state_topic), "state");
    mqtt_build_topic(events_topic, sizeof(events_topic), "events");
    mqtt_build_topic(warnings_topic, sizeof(warnings_topic), "warnings");

    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            snprintf(ap_ip_text, sizeof(ap_ip_text), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    if (esp_wifi_get_config(WIFI_IF_AP, &ap_cfg) == ESP_OK && ap_cfg.ap.ssid[0] != '\0') {
        snprintf(ap_ssid, sizeof(ap_ssid), "%s", (const char *)ap_cfg.ap.ssid);
    }

    payload = web_ui_json_build_connection_payload(&s_conn_cfg,
                                                   commands_topic,
                                                   state_topic,
                                                   events_topic,
                                                   warnings_topic,
                                                   ap_ssid,
                                                   ap_ip_text);
    if (!payload) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
    cJSON_free(payload);
    return err;
}

static esp_err_t connection_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    char value[128];
    char new_wifi_ssid[sizeof(s_conn_cfg.wifi_ssid)];
    char new_wifi_password[sizeof(s_conn_cfg.wifi_password)];
    bool wifi_ssid_updated = false;
    bool wifi_password_updated = false;
    esp_err_t status = ESP_OK;
    cJSON *root = NULL;

    body = (char *)calloc(1, (size_t)req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    if (read_request_body(req, body, (size_t)req->content_len + 1) != ESP_OK) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    root = web_ui_json_parse(body);
    if (!root) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    copy_bounded_local(new_wifi_ssid, sizeof(new_wifi_ssid), s_conn_cfg.wifi_ssid);
    copy_bounded_local(new_wifi_password, sizeof(new_wifi_password), s_conn_cfg.wifi_password);

    if (web_ui_json_get_string(root, "wifiSsid", value, sizeof(value))) {
        copy_bounded_local(new_wifi_ssid, sizeof(new_wifi_ssid), value);
        wifi_ssid_updated = true;
    }
    if (web_ui_json_get_string(root, "wifiPassword", value, sizeof(value))) {
        copy_bounded_local(new_wifi_password, sizeof(new_wifi_password), value);
        wifi_password_updated = true;
    }
    if (wifi_ssid_updated || wifi_password_updated) {
        wifi_ap_record_t ap_info;
        wifi_ap_record_t *ap_info_ptr = NULL;
        char validation_error[160];

        if (new_wifi_ssid[0] != '\0' && wifi_lookup_ap_record(new_wifi_ssid, &ap_info)) {
            ap_info_ptr = &ap_info;
            ESP_LOGI(TAG,
                     "Selected SSID '%s' auth=%s rssi=%d",
                     new_wifi_ssid,
                     wifi_authmode_to_str(ap_info.authmode),
                     (int)ap_info.rssi);
        }

        if (!validate_wifi_credentials(new_wifi_ssid,
                                       new_wifi_password,
                                       ap_info_ptr,
                                       validation_error,
                                       sizeof(validation_error))) {
            char err_payload[256];

            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            snprintf(err_payload,
                     sizeof(err_payload),
                     "{\"ok\":false,\"applied\":false,\"error\":\"%s\"}",
                     validation_error);
            status = httpd_resp_send(req, err_payload, HTTPD_RESP_USE_STRLEN);
            cJSON_Delete(root);
            free(body);
            return status;
        }

        copy_bounded_local(s_conn_cfg.wifi_ssid, sizeof(s_conn_cfg.wifi_ssid), new_wifi_ssid);
        copy_bounded_local(s_conn_cfg.wifi_password, sizeof(s_conn_cfg.wifi_password), new_wifi_password);
    }
    if (web_ui_json_get_string(root, "mqttHost", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_host, sizeof(s_conn_cfg.mqtt_host), value);
    }
    if (web_ui_json_get_int(root, "mqttPort", &s_conn_cfg.mqtt_port)) {
        if (s_conn_cfg.mqtt_port < 1 || s_conn_cfg.mqtt_port > 65535) {
            s_conn_cfg.mqtt_port = 1883;
        }
    }
    if (web_ui_json_get_string(root, "mqttUsername", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_username, sizeof(s_conn_cfg.mqtt_username), value);
    }
    if (web_ui_json_get_string(root, "mqttPassword", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_password, sizeof(s_conn_cfg.mqtt_password), value);
    }
    if (web_ui_json_get_string(root, "mqttBaseTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_base_topic, sizeof(s_conn_cfg.mqtt_base_topic), value);
    }
    if (web_ui_json_get_string(root, "mqttGameStateTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_game_state_topic, sizeof(s_conn_cfg.mqtt_game_state_topic), value);
    }
    if (web_ui_json_get_string(root, "mqttPropAnnounceTopic", value, sizeof(value)) ||
        web_ui_json_get_string(root, "mqttPropStateTopic", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.mqtt_prop_announce_topic,
                           sizeof(s_conn_cfg.mqtt_prop_announce_topic),
                           value);
    }
    if (web_ui_json_get_string(root, "networkName", value, sizeof(value))) {
        sanitize_network_name(value, s_conn_cfg.network_name, sizeof(s_conn_cfg.network_name));
        (void)apply_mdns_hostname();
    }
    if (web_ui_json_get_string(root, "apPassword", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.ap_password, sizeof(s_conn_cfg.ap_password), value);
    }
    {
        bool ap_val;
        if (web_ui_json_get_bool(root, "apEnabled", &ap_val)) {
            s_conn_cfg.ap_enabled = ap_val;
        }
    }

    ESP_LOGI(TAG, "Connection config updated (wifi ssid='%s', mqtt host='%s:%d', ap_enabled=%d)",
             s_conn_cfg.wifi_ssid, s_conn_cfg.mqtt_host, s_conn_cfg.mqtt_port, s_conn_cfg.ap_enabled);

    {
        esp_err_t save_err = save_connection_cfg_nvs();
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to persist connection config: %s", esp_err_to_name(save_err));
        }
    }

    mqtt_start_client();

    esp_err_t wifi_ret = ESP_OK;
    bool connect_attempted = false;
    if (s_conn_cfg.wifi_ssid[0] != '\0') {
        connect_attempted = true;
        wifi_ret = wifi_connect_sta(s_conn_cfg.wifi_ssid, s_conn_cfg.wifi_password);
    }

    httpd_resp_set_type(req, "application/json");
    if (connect_attempted && wifi_ret != ESP_OK) {
        char err_payload[160];
        snprintf(err_payload,
                 sizeof(err_payload),
                 "{\"ok\":false,\"applied\":true,\"connecting\":false,\"error\":\"%s\"}",
                 esp_err_to_name(wifi_ret));
        status = httpd_resp_send(req, err_payload, HTTPD_RESP_USE_STRLEN);
        cJSON_Delete(root);
        free(body);
        return status;
    }
    status = httpd_resp_sendstr(req,
                                connect_attempted
                                    ? "{\"ok\":true,\"applied\":true,\"connecting\":true}"
                                    : "{\"ok\":true,\"applied\":true,\"connecting\":false}");
    cJSON_Delete(root);
    free(body);
    return status;
}

static esp_err_t device_details_get_handler(httpd_req_t *req)
{
    char state_json[768] = {0};
    char game_state[32] = "unknown";
    int battery = -1;
    int64_t free_heap = (int64_t)esp_get_free_heap_size();
    const esp_app_desc_t *app = esp_app_get_description();
    char ip_text[32] = "unavailable";
    char ap_ip_text[32] = "192.168.4.1";
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            snprintf(ap_ip_text, sizeof(ap_ip_text), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    prop_engine_get_state_json(state_json, sizeof(state_json));
    (void)json_extract_int_local(state_json, "battery", &battery);
    (void)json_extract_string_local(state_json, "gameState", game_state, sizeof(game_state));

    char *payload;
    char wifi_ssid_json[64] = "";
    int wifi_rssi = 0;
    bool wifi_connected = false;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        wifi_connected = true;
        wifi_rssi = (int)ap_info.rssi;
        snprintf(wifi_ssid_json, sizeof(wifi_ssid_json), "%s", (const char *)ap_info.ssid);
        if (sta_netif) {
            esp_netif_ip_info_t sta_ip;
            if (esp_netif_get_ip_info(sta_netif, &sta_ip) == ESP_OK) {
                snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&sta_ip.ip));
            }
        }
    }

    payload = web_ui_json_build_device_details_payload(s_prop_id,
                                                       ip_text,
                                                       app->version,
                                                       app->version,
                                                       app->date,
                                                       app->time,
                                                       free_heap,
                                                       battery,
                                                       s_conn_cfg.network_name,
                                                       game_state,
                                                       ap_ip_text,
                                                       wifi_connected,
                                                       s_sta_connecting,
                                                       s_conn_cfg.wifi_ssid,
                                                       wifi_ssid_json,
                                                       wifi_rssi,
                                                       s_sta_last_error,
                                                       s_sta_last_disconnect_reason,
                                                       s_ap_shutdown_pending);
    if (!payload) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
    cJSON_free(payload);
    return err;
}

static esp_err_t device_name_post_handler(httpd_req_t *req)
{
    char body[256];
    char name_in[64];
    char *response;

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

    response = web_ui_json_build_device_name_payload(s_conn_cfg.network_name);
    if (!response) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    cJSON_free(response);
    return err;
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
                                "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d,\"authName\":\"%s\"}",
                                (const char *)records[i].ssid,
                                (int)records[i].rssi,
                                (int)records[i].authmode,
                                wifi_authmode_to_str(records[i].authmode));
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

static uint32_t get_backoff_ms(int retry_count)
{
    /* 1s, 2s, 4s, 8s, 16s, 32s cap */
    int shift = retry_count > 5 ? 5 : retry_count;
    return 1000u << shift;
}

static void sta_reconnect_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "STA reconnect attempt (retry=%d)", s_sta_retry_count);
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
    }
}

static void ap_shutdown_timer_cb(void *arg)
{
    s_ap_shutdown_pending = false;
    ESP_LOGI(TAG, "AP shutdown timer fired — switching to STA-only");
    esp_wifi_set_mode(WIFI_MODE_STA);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        int reason = event ? (int)event->reason : 0;

        s_sta_ip_text[0] = '\0';
        s_sta_last_disconnect_reason = reason;
        copy_bounded_local(s_sta_last_error,
                           sizeof(s_sta_last_error),
                           wifi_disconnect_reason_to_str(reason));

        /* Cancel pending AP shutdown — we lost the STA link */
        if (s_ap_shutdown_pending) {
            esp_timer_stop(s_ap_shutdown_timer);
            s_ap_shutdown_pending = false;
        }

        s_sta_connecting = (s_conn_cfg.wifi_ssid[0] != '\0');
        s_sta_retry_count++;
        ESP_LOGW(TAG,
                 "STA disconnected: reason=%d (%s), retry=%d",
                 reason,
                 s_sta_last_error,
                 s_sta_retry_count);

        if (s_sta_connecting && s_sta_reconnect_timer) {
            uint32_t delay_ms = get_backoff_ms(s_sta_retry_count - 1);
            ESP_LOGI(TAG, "STA reconnect in %lu ms", (unsigned long)delay_ms);
            esp_timer_start_once(s_sta_reconnect_timer, (uint64_t)delay_ms * 1000);
        }

        if (!s_conn_cfg.ap_enabled) {
            ESP_LOGI(TAG, "Re-enabling AP after STA disconnect");
            esp_wifi_set_mode(WIFI_MODE_APSTA);
        }

        mqtt_stop_client();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_connecting = false;
        s_sta_retry_count = 0;
        s_sta_last_disconnect_reason = 0;
        s_sta_last_error[0] = '\0';
        snprintf(s_sta_ip_text, sizeof(s_sta_ip_text), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "STA connected, IP=%s", s_sta_ip_text);

        /* Cancel any pending reconnect timer */
        if (s_sta_reconnect_timer) {
            esp_timer_stop(s_sta_reconnect_timer);
        }

        if (!s_conn_cfg.ap_enabled) {
            ESP_LOGI(TAG, "Scheduling AP shutdown in 10 seconds");
            s_ap_shutdown_pending = true;
            esp_timer_start_once(s_ap_shutdown_timer, 10000000);
        }

        mqtt_start_client();
    }
}

static esp_err_t wifi_connect_sta(const char *ssid, const char *password)
{
    wifi_ap_record_t ap_info;
    wifi_ap_record_t *ap_info_ptr = NULL;
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "wifi_connect_sta: empty SSID, skipping");
        return ESP_ERR_INVALID_ARG;
    }

    if (wifi_lookup_ap_record(ssid, &ap_info)) {
        ap_info_ptr = &ap_info;
    }

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (password && password[0] != '\0') {
        strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);
    }
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.threshold.authmode = wifi_select_sta_authmode(ap_info_ptr);
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;
#ifdef CONFIG_ESP_WIFI_ENABLE_WPA3_SAE
    sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#endif

    ESP_LOGI(TAG,
             "STA connecting to '%s' (auth=%s)",
             ssid,
             ap_info_ptr ? wifi_authmode_to_str(ap_info_ptr->authmode) : "unknown");
    s_sta_connecting = true;
    s_sta_retry_count = 0;
    s_sta_ip_text[0] = '\0';
    s_sta_last_disconnect_reason = 0;
    s_sta_last_error[0] = '\0';
    if (s_sta_reconnect_timer) {
        esp_timer_stop(s_sta_reconnect_timer);
    }

    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    return esp_wifi_connect();
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

    if (s_conn_cfg.ap_password[0] != '\0') {
        strncpy((char *)ap_cfg.ap.password, s_conn_cfg.ap_password, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

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

    {
        esp_err_t load_err = load_connection_cfg_nvs();
        if (load_err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded connection settings from /spiffs/config.json");
        } else {
            ESP_LOGI(TAG, "No saved connection settings yet (%s)", esp_err_to_name(load_err));
        }
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    /* Create timers before registering event handlers */
    {
        esp_timer_create_args_t reconnect_args = {
            .callback = sta_reconnect_timer_cb,
            .name = "sta_reconnect",
        };
        ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &s_sta_reconnect_timer));

        esp_timer_create_args_t ap_shutdown_args = {
            .callback = ap_shutdown_timer_cb,
            .name = "ap_shutdown",
        };
        ESP_ERROR_CHECK(esp_timer_create(&ap_shutdown_args, &s_ap_shutdown_timer));
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    ESP_ERROR_CHECK(start_softap());

    if (s_conn_cfg.wifi_ssid[0] != '\0') {
        esp_err_t wifi_ret = wifi_connect_sta(s_conn_cfg.wifi_ssid, s_conn_cfg.wifi_password);
        if (wifi_ret != ESP_OK) {
            ESP_LOGW(TAG, "Auto-connect to saved SSID failed: %s", esp_err_to_name(wifi_ret));
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 40;
    config.stack_size = 12288;

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

    httpd_uri_t update_html_uri = {
        .uri = "/update",
        .method = HTTP_GET,
        .handler = static_asset_handler,
        .user_ctx = (void *)&ASSET_UPDATE,
    };
    httpd_register_uri_handler(server, &update_html_uri);

    httpd_uri_t update_html2_uri = {
        .uri = "/update.html",
        .method = HTTP_GET,
        .handler = static_asset_handler,
        .user_ctx = (void *)&ASSET_UPDATE,
    };
    httpd_register_uri_handler(server, &update_html2_uri);

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

    httpd_uri_t ota_upload_uri = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = ota_upload_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &ota_upload_uri);

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

    xTaskCreate(mqtt_state_task, "mqtt_state", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

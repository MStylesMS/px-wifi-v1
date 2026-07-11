#include "web_ui.h"
#include "web_ui_json.h"
#include "prop_engine.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lib_json_helper.h"
#include "svc_wifi.h"
#include "svc_mqtt.h"
#include "svc_ota.h"

static const char *TAG = "web_ui";

/* Upper bound on the JSON request body accepted by config/connection POST
 * handlers. req->content_len is attacker-controlled input; without a cap an
 * oversized Content-Length lets a remote client force an arbitrarily large
 * heap allocation (calloc(1, content_len + 1)) and exhaust the device's
 * ~300KB heap with a single request. 16KB is generously above the largest
 * legitimate config/connection payload (a few KB of JSON) while still
 * bounding worst-case memory use. */
#define WEB_UI_MAX_JSON_BODY_LEN 16384

/* MQTT inbound work is queued off the MQTT client event task so command /
 * follower handling (locks, SPIFFS-adjacent paths, publishes) cannot stall
 * keepalive/reconnect. */
#define MQTT_INBOUND_QUEUE_LEN 8
#define MQTT_INBOUND_TOPIC_MAX 160
#define MQTT_INBOUND_PAYLOAD_MAX 768

typedef enum {
    MQTT_INBOUND_COMMAND = 0,
    MQTT_INBOUND_FOLLOWER = 1,
} mqtt_inbound_kind_t;

typedef struct {
    mqtt_inbound_kind_t kind;
    int payload_len;
    char payload[MQTT_INBOUND_PAYLOAD_MAX];
} mqtt_inbound_msg_t;

static bool json_extract_string_local(const char *json, const char *key, char *out, size_t out_size);
static bool json_extract_int_local(const char *json, const char *key, int *out);
static bool json_extract_bool_local(const char *json, const char *key, bool *out);
static void json_escape_string_local(const char *src, char *dst, size_t dst_size);
static void copy_bounded_local(char *dst, size_t dst_size, const char *src);
static void trim_whitespace_inplace(char *s);
static void sanitize_network_name(const char *src, char *out, size_t out_size);
static esp_err_t apply_mdns_hostname(void);
static esp_err_t save_connection_cfg_nvs(void);
static void mqtt_start_client(void);
static void mqtt_stop_client(void);
static void ota_reboot_task(void *arg);
static bool conn_cfg_lock(void);
static void conn_cfg_unlock(void);
static void conn_cfg_snapshot(connection_cfg_t *out);
static esp_err_t apply_connection_fields_from_json(const cJSON *root,
                                                   bool *out_wifi_changed,
                                                   char *validation_error,
                                                   size_t validation_error_size);
static esp_err_t send_json_error(httpd_req_t *req,
                                 const char *http_status,
                                 const char *error_text);

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

static connection_cfg_t s_conn_cfg = WEB_UI_CONNECTION_CFG_DEFAULT;
static SemaphoreHandle_t s_conn_cfg_mutex;
static QueueHandle_t s_mqtt_inbound_queue;

static bool conn_cfg_lock(void)
{
    if (!s_conn_cfg_mutex) {
        return true;
    }
    return xSemaphoreTake(s_conn_cfg_mutex, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void conn_cfg_unlock(void)
{
    if (s_conn_cfg_mutex) {
        xSemaphoreGive(s_conn_cfg_mutex);
    }
}

static void conn_cfg_snapshot(connection_cfg_t *out)
{
    if (!out) {
        return;
    }
    if (!conn_cfg_lock()) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_conn_cfg;
    conn_cfg_unlock();
}

static esp_err_t load_connection_cfg_nvs(void)
{
    esp_err_t err;

    if (!conn_cfg_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    err = web_ui_json_load_connection_cfg(WEB_UI_CONFIG_FILE_PATH, &s_conn_cfg);
    /* SoftAP stays up after STA connect; PSK is the access control. */
    s_conn_cfg.ap_enabled = true;
    conn_cfg_unlock();
    return err;
}

static esp_err_t save_connection_cfg_nvs(void)
{
    char prop_cfg[4096];
    connection_cfg_t cfg_copy;
    esp_err_t err;

    prop_engine_get_config_json(prop_cfg, sizeof(prop_cfg));
    if (!conn_cfg_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    cfg_copy = s_conn_cfg;
    conn_cfg_unlock();
    return web_ui_json_save_connection_cfg(WEB_UI_CONFIG_FILE_PATH, &cfg_copy, prop_cfg);
}

static esp_err_t send_json_error(httpd_req_t *req,
                                 const char *http_status,
                                 const char *error_text)
{
    char err_payload[320];
    char escaped[193];

    json_escape_string_local(error_text ? error_text : "error", escaped, sizeof(escaped));
    snprintf(err_payload,
             sizeof(err_payload),
             "{\"ok\":false,\"error\":\"%s\"}",
             escaped);
    if (http_status && http_status[0] != '\0') {
        httpd_resp_set_status(req, http_status);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, err_payload, HTTPD_RESP_USE_STRLEN);
}

/* Shared connection-field apply used by both /api/config and /api/connection.
 * On validation failure returns ESP_ERR_INVALID_ARG and fills validation_error.
 * out_wifi_changed is set when wifiSsid/wifiPassword were present and accepted. */
static esp_err_t apply_connection_fields_from_json(const cJSON *root,
                                                   bool *out_wifi_changed,
                                                   char *validation_error,
                                                   size_t validation_error_size)
{
    char value[128];
    char new_wifi_ssid[sizeof(s_conn_cfg.wifi_ssid)];
    char new_wifi_password[sizeof(s_conn_cfg.wifi_password)];
    bool wifi_ssid_updated = false;
    bool wifi_password_updated = false;
    bool wifi_changed = false;

    if (out_wifi_changed) {
        *out_wifi_changed = false;
    }
    if (validation_error && validation_error_size > 0) {
        validation_error[0] = '\0';
    }
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!conn_cfg_lock()) {
        if (validation_error && validation_error_size > 0) {
            snprintf(validation_error, validation_error_size, "busy");
        }
        return ESP_ERR_TIMEOUT;
    }

    copy_bounded_local(new_wifi_ssid, sizeof(new_wifi_ssid), s_conn_cfg.wifi_ssid);
    copy_bounded_local(new_wifi_password, sizeof(new_wifi_password), s_conn_cfg.wifi_password);

    if (web_ui_json_get_string(root, "wifiSsid", value, sizeof(value))) {
        copy_bounded_local(new_wifi_ssid, sizeof(new_wifi_ssid), value);
        wifi_ssid_updated = true;
    }
    if (web_ui_json_get_string(root, "wifiPassword", value, sizeof(value))) {
        trim_whitespace_inplace(value);
        copy_bounded_local(new_wifi_password, sizeof(new_wifi_password), value);
        wifi_password_updated = true;
    }

    if (wifi_ssid_updated || wifi_password_updated) {
        wifi_ap_record_t ap_info;
        wifi_ap_record_t *ap_info_ptr = NULL;
        char local_validation_error[160];

        if (new_wifi_ssid[0] != '\0' && svc_wifi_lookup_ap_record(new_wifi_ssid, &ap_info)) {
            ap_info_ptr = &ap_info;
            ESP_LOGI(TAG,
                     "Selected SSID '%s' auth=%s rssi=%d",
                     new_wifi_ssid,
                     svc_wifi_authmode_to_str(ap_info.authmode),
                     (int)ap_info.rssi);
        }

        if (!svc_wifi_validate_credentials(new_wifi_ssid,
                                           new_wifi_password,
                                           ap_info_ptr,
                                           local_validation_error,
                                           sizeof(local_validation_error))) {
            if (validation_error && validation_error_size > 0) {
                copy_bounded_local(validation_error, validation_error_size, local_validation_error);
            }
            conn_cfg_unlock();
            return ESP_ERR_INVALID_ARG;
        }

        copy_bounded_local(s_conn_cfg.wifi_ssid, sizeof(s_conn_cfg.wifi_ssid), new_wifi_ssid);
        copy_bounded_local(s_conn_cfg.wifi_password, sizeof(s_conn_cfg.wifi_password), new_wifi_password);
        wifi_changed = true;
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
    }
    if (web_ui_json_get_string(root, "apPassword", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.ap_password, sizeof(s_conn_cfg.ap_password), value);
    }
    /* SoftAP remains enabled after STA connect; PSK is the access control.
     * Ignore client attempts to disable it so config/connection stay aligned. */
    s_conn_cfg.ap_enabled = true;
    svc_wifi_set_ap_enabled(true);

    ESP_LOGI(TAG,
             "Connection config updated (wifi ssid='%s', mqtt host='%s:%d', ap_enabled=%d)",
             s_conn_cfg.wifi_ssid,
             s_conn_cfg.mqtt_host,
             s_conn_cfg.mqtt_port,
             s_conn_cfg.ap_enabled);
    conn_cfg_unlock();

    (void)apply_mdns_hostname();

    if (out_wifi_changed) {
        *out_wifi_changed = wifi_changed;
    }
    return ESP_OK;
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

    if (!conn_cfg_lock()) {
        return;
    }
    if (s_conn_cfg.network_name[0] == '\0') {
        snprintf(s_conn_cfg.network_name, sizeof(s_conn_cfg.network_name), "%s", s_prop_id);
    }
    conn_cfg_unlock();
}

static esp_err_t apply_mdns_hostname(void)
{
    char host[33] = {0};
    char network_name[sizeof(s_conn_cfg.network_name)];

    if (!conn_cfg_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    copy_bounded_local(network_name, sizeof(network_name), s_conn_cfg.network_name);
    sanitize_network_name(network_name, host, sizeof(host));
    copy_bounded_local(s_conn_cfg.network_name, sizeof(s_conn_cfg.network_name), host);
    conn_cfg_unlock();

    return svc_wifi_set_hostname(host, s_prop_id);
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
    lib_json_escape_string(src, dst, dst_size);
}

static void copy_bounded_local(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/* Strips leading/trailing ASCII whitespace in place. Guards against a stray
 * space silently corrupting a WiFi PSK typed/pasted from a phone keyboard
 * (autocapitalize/autocorrect can also add a trailing space), which would
 * otherwise associate fine but fail the WPA2 4-way handshake with no obvious
 * indication why. */
static void trim_whitespace_inplace(char *s)
{
    size_t len;
    size_t start = 0;

    if (!s) {
        return;
    }

    len = strlen(s);
    while (start < len && isspace((unsigned char)s[start])) {
        start++;
    }
    if (start > 0) {
        memmove(s, s + start, len - start + 1);
        len -= start;
    }
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

/* Caller must hold conn_cfg lock, or accept a best-effort read of a
 * stable default when the mutex is unavailable. */
static const char *mqtt_base_topic_or_default(void)
{
    return s_conn_cfg.mqtt_base_topic[0] != '\0' ? s_conn_cfg.mqtt_base_topic : "site/room/zone";
}

static void mqtt_build_topic_locked(char *out, size_t out_size, const char *suffix)
{
    snprintf(out, out_size, "%s/%s", mqtt_base_topic_or_default(), suffix ? suffix : "");
}

static const char *prop_led_hint_name_local(prop_led_hint_t hint);

static void mqtt_build_topic(char *out, size_t out_size, const char *suffix)
{
    if (!conn_cfg_lock()) {
        snprintf(out, out_size, "site/room/zone/%s", suffix ? suffix : "");
        return;
    }
    mqtt_build_topic_locked(out, out_size, suffix);
    conn_cfg_unlock();
}

/* Thin wrapper around svc_mqtt_publish() that logs a warning when the
 * publish is dropped (e.g. client queue full / invalid args), instead of
 * silently discarding the failure like every call site used to. */
static void mqtt_publish_or_warn(const char *topic, const char *payload, int qos, bool retain)
{
    if (svc_mqtt_publish(topic, payload, qos, retain) != ESP_OK) {
        ESP_LOGW(TAG, "MQTT publish dropped (topic=%s)", topic ? topic : "(null)");
    }
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

    if (!svc_mqtt_is_connected()) {
        return;
    }

    mqtt_build_topic(topic, sizeof(topic), "state");
    prop_engine_get_state_json(state_json, sizeof(state_json));
    mqtt_publish_or_warn(topic, state_json, 1, retained);
}

static bool mqtt_publish_engine_events(bool publish_state_after)
{
    char event_json[384];
    char topic[160];
    bool had_events = false;

    if (!svc_mqtt_is_connected()) {
        return false;
    }

    mqtt_build_topic(topic, sizeof(topic), "events");
    while (prop_engine_pop_event_json(event_json, sizeof(event_json))) {
        mqtt_publish_or_warn(topic, event_json, 1, false);
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
    char state_topic[160];
    char commands_topic[160];
    char announce_topic[sizeof(s_conn_cfg.mqtt_prop_announce_topic)];
    char host[33] = {0};
    char network_name[sizeof(s_conn_cfg.network_name)];
    char ip_text[32] = "unavailable";
    prop_battery_snapshot_t battery_snap;
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    const esp_app_desc_t *app = esp_app_get_description();

    if (!svc_mqtt_is_connected()) {
        return;
    }

    if (!conn_cfg_lock()) {
        return;
    }
    if (s_conn_cfg.mqtt_prop_announce_topic[0] == '\0') {
        conn_cfg_unlock();
        return;
    }
    copy_bounded_local(announce_topic, sizeof(announce_topic), s_conn_cfg.mqtt_prop_announce_topic);
    copy_bounded_local(network_name, sizeof(network_name), s_conn_cfg.network_name);
    mqtt_build_topic_locked(state_topic, sizeof(state_topic), "state");
    mqtt_build_topic_locked(commands_topic, sizeof(commands_topic), "commands");
    conn_cfg_unlock();

    if (sta_netif) {
        esp_netif_ip_info_t sta_ip;
        if (esp_netif_get_ip_info(sta_netif, &sta_ip) == ESP_OK) {
            snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&sta_ip.ip));
        }
    }

    prop_engine_get_battery_snapshot(&battery_snap);

    sanitize_network_name(network_name, host, sizeof(host));
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
             battery_snap.wire_count,
             battery_snap.battery_adc_raw,
             battery_snap.battery_adc_at_0v,
             battery_snap.battery_adc_at_15v,
             battery_snap.battery_profile,
             prop_led_hint_name_local(prop_engine_get_led_hint()),
             app->version,
             app->version,
             app->date,
             app->time,
             state_topic,
             commands_topic);

    mqtt_publish_or_warn(announce_topic, announce, 1, false);
}

static void mqtt_publish_warning(const char *message)
{
    char payload[384];
    char topic[160];
    char warning_json[193];

    if (!svc_mqtt_is_connected()) {
        return;
    }

    mqtt_build_topic(topic, sizeof(topic), "warnings");
    json_escape_string_local(message ? message : "unknown", warning_json, sizeof(warning_json));

    snprintf(payload,
             sizeof(payload),
             "{\"ts\":%lld,\"warning\":\"%s\"}",
             (long long)(esp_timer_get_time() / 1000),
             warning_json);
    mqtt_publish_or_warn(topic, payload, 1, false);
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

    if (!svc_mqtt_is_connected()) {
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
    mqtt_publish_or_warn(topic, payload, 1, false);
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
        ESP_LOGW(TAG, "mqtt_apply_follower_payload: message truncated from %d to %d bytes",
                 payload_len, (int)sizeof(msg) - 1);
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
        mqtt_publish_or_warn(events_topic, response, 1, false);
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

static void mqtt_on_connected(void *ctx)
{
    bool follow_enabled = false;
    int unused_tol = 0;
    char commands_topic[160];
    char game_state_topic[sizeof(s_conn_cfg.mqtt_game_state_topic)];

    (void)ctx;

    mqtt_build_topic(commands_topic, sizeof(commands_topic), "commands");
    svc_mqtt_subscribe(commands_topic, 1);
    mqtt_get_follow_cfg(&follow_enabled, &unused_tol);

    game_state_topic[0] = '\0';
    if (conn_cfg_lock()) {
        copy_bounded_local(game_state_topic, sizeof(game_state_topic), s_conn_cfg.mqtt_game_state_topic);
        conn_cfg_unlock();
    }
    if (follow_enabled && game_state_topic[0] != '\0') {
        svc_mqtt_subscribe(game_state_topic, 1);
    }
    mqtt_publish_announce();
    mqtt_publish_state(true);
}

static void mqtt_on_disconnected(void *ctx)
{
    (void)ctx;
}

static void mqtt_inbound_worker(void *arg)
{
    mqtt_inbound_msg_t msg;

    (void)arg;
    while (true) {
        if (xQueueReceive(s_mqtt_inbound_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.kind == MQTT_INBOUND_COMMAND) {
            mqtt_handle_command_payload(msg.payload, msg.payload_len);
        } else {
            mqtt_apply_follower_payload(msg.payload, msg.payload_len);
        }
    }
}

static bool mqtt_enqueue_inbound(mqtt_inbound_kind_t kind, const char *data, size_t data_len)
{
    mqtt_inbound_msg_t msg;

    if (!s_mqtt_inbound_queue || !data || data_len == 0) {
        return false;
    }

    memset(&msg, 0, sizeof(msg));
    msg.kind = kind;
    msg.payload_len = (int)data_len;
    if (msg.payload_len >= (int)sizeof(msg.payload)) {
        msg.payload_len = (int)sizeof(msg.payload) - 1;
    }
    memcpy(msg.payload, data, (size_t)msg.payload_len);
    msg.payload[msg.payload_len] = '\0';

    if (xQueueSend(s_mqtt_inbound_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "MQTT inbound queue full; dropping %s message",
                 kind == MQTT_INBOUND_COMMAND ? "command" : "follower");
        return false;
    }
    return true;
}

static void mqtt_on_message(const char *topic, size_t topic_len,
                            const char *data, size_t data_len, void *ctx)
{
    char commands_topic[160];
    char game_state_topic[sizeof(s_conn_cfg.mqtt_game_state_topic)];

    (void)ctx;

    /* Keep this callback short: only classify + enqueue. Heavy work runs on
     * mqtt_inbound_worker so the MQTT client task can service keepalive. */
    mqtt_build_topic(commands_topic, sizeof(commands_topic), "commands");
    game_state_topic[0] = '\0';
    if (conn_cfg_lock()) {
        copy_bounded_local(game_state_topic, sizeof(game_state_topic), s_conn_cfg.mqtt_game_state_topic);
        conn_cfg_unlock();
    }

    if (topic_len == strlen(commands_topic) &&
        strncmp(topic, commands_topic, topic_len) == 0) {
        (void)mqtt_enqueue_inbound(MQTT_INBOUND_COMMAND, data, data_len);
    } else if (game_state_topic[0] != '\0' &&
               topic_len == strlen(game_state_topic) &&
               strncmp(topic, game_state_topic, topic_len) == 0) {
        (void)mqtt_enqueue_inbound(MQTT_INBOUND_FOLLOWER, data, data_len);
    }
}

static void mqtt_stop_client(void)
{
    svc_mqtt_stop();
}

static void mqtt_start_client(void)
{
    svc_mqtt_config_t mqtt_cfg = SVC_MQTT_CONFIG_DEFAULT();
    connection_cfg_t cfg;
    static char host_buf[sizeof(cfg.mqtt_host)];
    static char user_buf[sizeof(cfg.mqtt_username)];
    static char pass_buf[sizeof(cfg.mqtt_password)];

    conn_cfg_snapshot(&cfg);
    if (cfg.mqtt_host[0] == '\0') {
        svc_mqtt_stop();
        ESP_LOGI(TAG, "MQTT host empty; MQTT client not started");
        return;
    }

    copy_bounded_local(host_buf, sizeof(host_buf), cfg.mqtt_host);
    copy_bounded_local(user_buf, sizeof(user_buf), cfg.mqtt_username);
    copy_bounded_local(pass_buf, sizeof(pass_buf), cfg.mqtt_password);

    mqtt_cfg.host = host_buf;
    mqtt_cfg.port = cfg.mqtt_port;
    mqtt_cfg.client_id = s_prop_id;
    mqtt_cfg.username = user_buf[0] != '\0' ? user_buf : NULL;
    mqtt_cfg.password = pass_buf[0] != '\0' ? pass_buf : NULL;
    svc_mqtt_start(&mqtt_cfg);
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

        if (!svc_mqtt_is_connected()) {
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
    connection_cfg_t cfg_snap;

    prop_engine_get_config_json(prop_payload, sizeof(prop_payload));

    mqtt_build_topic(commands_topic, sizeof(commands_topic), "commands");
    mqtt_build_topic(state_topic, sizeof(state_topic), "state");
    mqtt_build_topic(events_topic, sizeof(events_topic), "events");
    mqtt_build_topic(warnings_topic, sizeof(warnings_topic), "warnings");
    conn_cfg_snapshot(&cfg_snap);

    payload = web_ui_json_build_unified_config_payload(&cfg_snap,
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
    esp_err_t engine_err;

    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    (void)json_extract_string_local(body, "command", command_name, sizeof(command_name));
    engine_err = prop_engine_handle_command_json(body, response, sizeof(response));
    if (engine_err == ESP_ERR_TIMEOUT) {
        return send_json_error(req, "503 Service Unavailable", "busy");
    }
    if (engine_err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(engine_err));
    }

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

static int ota_read_from_httpd_req(void *ctx, uint8_t *buf, size_t buf_size)
{
    httpd_req_t *req = (httpd_req_t *)ctx;
    return httpd_req_recv(req, (char *)buf, buf_size);
}

static esp_err_t ota_upload_post_handler(httpd_req_t *req)
{
    esp_err_t err;

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing firmware payload");
        return ESP_FAIL;
    }

    err = svc_ota_apply((size_t)req->content_len, ota_read_from_httpd_req, req);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return err;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA update failed");
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
    char validation_error[160];
    bool persist = strstr(req->uri, "/save") != NULL;
    bool wifi_changed = false;
    esp_err_t status = ESP_OK;
    esp_err_t engine_err;
    esp_err_t conn_err;
    cJSON *root = NULL;
    connection_cfg_t cfg_snap;

    if (req->content_len <= 0 || req->content_len > WEB_UI_MAX_JSON_BODY_LEN) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Request body too large");
        return ESP_FAIL;
    }

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

    engine_err = prop_engine_apply_config_json(body, persist, response, sizeof(response));
    if (engine_err == ESP_ERR_TIMEOUT) {
        cJSON_Delete(root);
        free(body);
        return send_json_error(req, "503 Service Unavailable", "busy");
    }
    if (engine_err != ESP_OK) {
        cJSON_Delete(root);
        free(body);
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(engine_err));
    }

    conn_err = apply_connection_fields_from_json(root,
                                                 &wifi_changed,
                                                 validation_error,
                                                 sizeof(validation_error));
    if (conn_err == ESP_ERR_TIMEOUT) {
        cJSON_Delete(root);
        free(body);
        return send_json_error(req, "503 Service Unavailable", "busy");
    }
    if (conn_err == ESP_ERR_INVALID_ARG) {
        cJSON_Delete(root);
        free(body);
        return send_json_error(req, "400 Bad Request",
                               validation_error[0] ? validation_error : "invalidWifiCredentials");
    }
    if (conn_err != ESP_OK) {
        cJSON_Delete(root);
        free(body);
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(conn_err));
    }

    if (persist) {
        esp_err_t save_err = save_connection_cfg_nvs();
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to persist unified config: %s", esp_err_to_name(save_err));
        }
    }

    mqtt_start_client();

    conn_cfg_snapshot(&cfg_snap);
    if (wifi_changed && cfg_snap.wifi_ssid[0] != '\0') {
        esp_err_t wifi_ret = svc_wifi_connect_sta(cfg_snap.wifi_ssid, cfg_snap.wifi_password);
        if (wifi_ret != ESP_OK) {
            ESP_LOGW(TAG, "STA reconnect after config POST failed: %s", esp_err_to_name(wifi_ret));
        }
    }

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
    esp_err_t engine_err;

    engine_err = prop_engine_restore_defaults(false, response, sizeof(response));
    if (engine_err == ESP_ERR_TIMEOUT) {
        return send_json_error(req, "503 Service Unavailable", "busy");
    }
    if (engine_err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(engine_err));
    }
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
    connection_cfg_t cfg_snap;
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    mqtt_build_topic(commands_topic, sizeof(commands_topic), "commands");
    mqtt_build_topic(state_topic, sizeof(state_topic), "state");
    mqtt_build_topic(events_topic, sizeof(events_topic), "events");
    mqtt_build_topic(warnings_topic, sizeof(warnings_topic), "warnings");
    conn_cfg_snapshot(&cfg_snap);

    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            snprintf(ap_ip_text, sizeof(ap_ip_text), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    {
        char scanned_ssid[33];
        if (svc_wifi_get_ap_ssid(scanned_ssid, sizeof(scanned_ssid)) == ESP_OK) {
            copy_bounded_local(ap_ssid, sizeof(ap_ssid), scanned_ssid);
        }
    }

    payload = web_ui_json_build_connection_payload(&cfg_snap,
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
    char validation_error[160];
    bool wifi_changed = false;
    esp_err_t status = ESP_OK;
    esp_err_t conn_err;
    cJSON *root = NULL;
    connection_cfg_t cfg_snap;

    if (req->content_len <= 0 || req->content_len > WEB_UI_MAX_JSON_BODY_LEN) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Request body too large");
        return ESP_FAIL;
    }

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

    conn_err = apply_connection_fields_from_json(root,
                                                 &wifi_changed,
                                                 validation_error,
                                                 sizeof(validation_error));
    if (conn_err == ESP_ERR_TIMEOUT) {
        cJSON_Delete(root);
        free(body);
        return send_json_error(req, "503 Service Unavailable", "busy");
    }
    if (conn_err == ESP_ERR_INVALID_ARG) {
        char err_payload[320];
        char escaped[193];

        json_escape_string_local(validation_error[0] ? validation_error : "invalidWifiCredentials",
                                 escaped,
                                 sizeof(escaped));
        snprintf(err_payload,
                 sizeof(err_payload),
                 "{\"ok\":false,\"applied\":false,\"error\":\"%s\"}",
                 escaped);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        status = httpd_resp_send(req, err_payload, HTTPD_RESP_USE_STRLEN);
        cJSON_Delete(root);
        free(body);
        return status;
    }
    if (conn_err != ESP_OK) {
        cJSON_Delete(root);
        free(body);
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(conn_err));
    }

    {
        esp_err_t save_err = save_connection_cfg_nvs();
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to persist connection config: %s", esp_err_to_name(save_err));
        }
    }

    mqtt_start_client();

    conn_cfg_snapshot(&cfg_snap);
    esp_err_t wifi_ret = ESP_OK;
    bool connect_attempted = false;
    if (cfg_snap.wifi_ssid[0] != '\0') {
        connect_attempted = true;
        wifi_ret = svc_wifi_connect_sta(cfg_snap.wifi_ssid, cfg_snap.wifi_password);
    }

    httpd_resp_set_type(req, "application/json");
    if (connect_attempted && wifi_ret != ESP_OK) {
        char err_payload[192];
        char escaped[96];

        json_escape_string_local(esp_err_to_name(wifi_ret), escaped, sizeof(escaped));
        snprintf(err_payload,
                 sizeof(err_payload),
                 "{\"ok\":false,\"applied\":true,\"connecting\":false,\"error\":\"%s\"}",
                 escaped);
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
    char battery_state[16] = "normal";
    int battery = -1;
    int battery_voltage_mv = 0;
    bool battery_low = false;
    int64_t free_heap = (int64_t)esp_get_free_heap_size();
    const esp_app_desc_t *app = esp_app_get_description();
    char ap_ip_text[32] = "192.168.4.1";
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            snprintf(ap_ip_text, sizeof(ap_ip_text), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    prop_engine_get_state_json(state_json, sizeof(state_json));
    (void)json_extract_int_local(state_json, "battery", &battery);
    (void)json_extract_string_local(state_json, "gameState", game_state, sizeof(game_state));
    (void)json_extract_string_local(state_json, "batteryState", battery_state, sizeof(battery_state));
    (void)json_extract_int_local(state_json, "batteryVoltageMv", &battery_voltage_mv);
    (void)json_extract_bool_local(state_json, "lowBattery", &battery_low);

    char *payload;
    svc_wifi_status_t wifi_status;
    svc_wifi_get_status(&wifi_status);

    connection_cfg_t cfg_snap;
    conn_cfg_snapshot(&cfg_snap);

    payload = web_ui_json_build_device_details_payload(s_prop_id,
                                                       wifi_status.ip_text,
                                                       app->version,
                                                       app->version,
                                                       app->date,
                                                       app->time,
                                                       free_heap,
                                                       battery,
                                                       battery_state,
                                                       battery_voltage_mv,
                                                       battery_low,
                                                       cfg_snap.network_name,
                                                       game_state,
                                                       ap_ip_text,
                                                       wifi_status.connected,
                                                       wifi_status.connecting,
                                                       cfg_snap.wifi_ssid,
                                                       wifi_status.ssid,
                                                       wifi_status.rssi,
                                                       wifi_status.last_error,
                                                       wifi_status.last_disconnect_reason,
                                                       wifi_status.ap_shutdown_pending);
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
    char network_name[sizeof(s_conn_cfg.network_name)];
    char *response;

    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    if (!json_extract_string_local(body, "networkName", name_in, sizeof(name_in))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing networkName");
        return ESP_FAIL;
    }

    if (!conn_cfg_lock()) {
        return send_json_error(req, "503 Service Unavailable", "busy");
    }
    sanitize_network_name(name_in, s_conn_cfg.network_name, sizeof(s_conn_cfg.network_name));
    copy_bounded_local(network_name, sizeof(network_name), s_conn_cfg.network_name);
    conn_cfg_unlock();

    (void)apply_mdns_hostname();

    {
        esp_err_t save_err = save_connection_cfg_nvs();
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to persist device name: %s", esp_err_to_name(save_err));
        }
    }

    if (conn_cfg_lock()) {
        copy_bounded_local(network_name, sizeof(network_name), s_conn_cfg.network_name);
        conn_cfg_unlock();
    }

    response = web_ui_json_build_device_name_payload(network_name);
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
    char *payload;
    size_t pos = 0;
    char ssid_escaped[96];
    char auth_escaped[48];

    esp_err_t err = svc_wifi_scan_networks(records, &count);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"scanFailed\",\"networks\":[]}");
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
        json_escape_string_local((const char *)records[i].ssid, ssid_escaped, sizeof(ssid_escaped));
        json_escape_string_local(svc_wifi_authmode_to_str(records[i].authmode),
                                 auth_escaped,
                                 sizeof(auth_escaped));
        pos += (size_t)snprintf(payload + pos,
                                4096 - pos,
                                "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d,\"authName\":\"%s\"}",
                                ssid_escaped,
                                (int)records[i].rssi,
                                (int)records[i].authmode,
                                auth_escaped);
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
        esp_err_t engine_err = prop_engine_handle_command_json(payload, reply_buf, sizeof(reply_buf));
        if (engine_err == ESP_ERR_TIMEOUT) {
            snprintf(reply_buf, sizeof(reply_buf), "{\"ok\":false,\"error\":\"busy\"}");
        } else if (engine_err != ESP_OK) {
            snprintf(reply_buf,
                     sizeof(reply_buf),
                     "{\"ok\":false,\"error\":\"%s\"}",
                     esp_err_to_name(engine_err));
        }
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

static void wifi_on_sta_connected(const char *ip_text, void *ctx)
{
    (void)ctx;
    (void)ip_text;
    mqtt_start_client();
}

static void wifi_on_sta_disconnected(int reason, const char *reason_text, void *ctx)
{
    (void)ctx;
    (void)reason;
    (void)reason_text;
    mqtt_stop_client();
}

esp_err_t web_ui_start(void)
{
    connection_cfg_t cfg_snap;
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (!s_conn_cfg_mutex) {
        s_conn_cfg_mutex = xSemaphoreCreateMutex();
        if (!s_conn_cfg_mutex) {
            ESP_LOGE(TAG, "Failed to create connection config mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_mqtt_inbound_queue) {
        s_mqtt_inbound_queue = xQueueCreate(MQTT_INBOUND_QUEUE_LEN, sizeof(mqtt_inbound_msg_t));
        if (!s_mqtt_inbound_queue) {
            ESP_LOGE(TAG, "Failed to create MQTT inbound queue");
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreate(mqtt_inbound_worker, "mqtt_in", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MQTT inbound worker");
        return ESP_ERR_NO_MEM;
    }

    {
        esp_err_t load_err = load_connection_cfg_nvs();
        if (load_err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded connection settings from /spiffs/config.json");
        } else {
            ESP_LOGI(TAG, "No saved connection settings yet (%s)", esp_err_to_name(load_err));
        }
    }

    conn_cfg_snapshot(&cfg_snap);
    /* SoftAP stays up after STA connect; PSK is the access control. */
    cfg_snap.ap_enabled = true;

    {
        uint8_t mac[6] = {0};
        char ap_ssid[33];
        static char ap_password_buf[sizeof(cfg_snap.ap_password)];
        svc_wifi_config_t wifi_cfg = SVC_WIFI_CONFIG_DEFAULT();

        (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(ap_ssid, sizeof(ap_ssid), "Paradox-PXWiFiV1-%02X%02X", mac[4], mac[5]);
        copy_bounded_local(ap_password_buf, sizeof(ap_password_buf), cfg_snap.ap_password);
        wifi_cfg.ap_ssid = ap_ssid;
        wifi_cfg.ap_password = ap_password_buf;
        wifi_cfg.ap_enabled = true;

        ESP_ERROR_CHECK(svc_wifi_init(&wifi_cfg));
        svc_wifi_set_ap_enabled(true);
    }

    svc_wifi_set_sta_connected_cb(wifi_on_sta_connected, NULL);
    svc_wifi_set_sta_disconnected_cb(wifi_on_sta_disconnected, NULL);
    svc_mqtt_set_connected_cb(mqtt_on_connected, NULL);
    svc_mqtt_set_disconnected_cb(mqtt_on_disconnected, NULL);
    svc_mqtt_set_message_cb(mqtt_on_message, NULL);

    build_default_identity();
    (void)apply_mdns_hostname();

    conn_cfg_snapshot(&cfg_snap);
    if (cfg_snap.wifi_ssid[0] != '\0') {
        esp_err_t wifi_ret = svc_wifi_connect_sta(cfg_snap.wifi_ssid, cfg_snap.wifi_password);
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

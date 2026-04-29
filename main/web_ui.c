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
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "web_ui";

static esp_err_t wifi_connect_sta(const char *ssid, const char *password);

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
    .cache_control = "no-store",
};

static const static_asset_t ASSET_APP_JS = {
    .start = app_js_start,
    .end = app_js_end,
    .content_type = "application/javascript; charset=utf-8",
    .cache_control = "no-store",
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
    char ap_password[65];
    bool ap_enabled;
} connection_cfg_t;

static char s_prop_id[32] = "px-wifi-v1";
static bool s_mdns_started;
static bool s_sta_connecting;
static int s_sta_retry_count;
static esp_timer_handle_t s_sta_reconnect_timer;
static esp_timer_handle_t s_ap_shutdown_timer;
static bool s_ap_shutdown_pending;
static char s_sta_ip_text[32] = "";
static int s_sta_last_disconnect_reason;
static char s_sta_last_error[48] = "";

#define CONN_STORE_NS "web_ui"
#define CONN_STORE_KEY "conn_cfg_v1"

typedef struct {
    uint32_t version;
    connection_cfg_t cfg;
} connection_store_t;

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
    .ap_password = "",
    .ap_enabled = true,
};

static esp_err_t load_connection_cfg_nvs(void)
{
    nvs_handle_t nvs = 0;
    size_t req_size = sizeof(connection_store_t);
    connection_store_t stored;

    esp_err_t err = nvs_open(CONN_STORE_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(nvs, CONN_STORE_KEY, &stored, &req_size);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (req_size != sizeof(connection_store_t) || stored.version != 1) {
        return ESP_ERR_INVALID_VERSION;
    }

    s_conn_cfg = stored.cfg;
    s_conn_cfg.wifi_ssid[sizeof(s_conn_cfg.wifi_ssid) - 1] = '\0';
    s_conn_cfg.wifi_password[sizeof(s_conn_cfg.wifi_password) - 1] = '\0';
    s_conn_cfg.mqtt_host[sizeof(s_conn_cfg.mqtt_host) - 1] = '\0';
    s_conn_cfg.mqtt_username[sizeof(s_conn_cfg.mqtt_username) - 1] = '\0';
    s_conn_cfg.mqtt_password[sizeof(s_conn_cfg.mqtt_password) - 1] = '\0';
    s_conn_cfg.mqtt_base_topic[sizeof(s_conn_cfg.mqtt_base_topic) - 1] = '\0';
    s_conn_cfg.mqtt_commands_topic[sizeof(s_conn_cfg.mqtt_commands_topic) - 1] = '\0';
    s_conn_cfg.mqtt_state_topic[sizeof(s_conn_cfg.mqtt_state_topic) - 1] = '\0';
    s_conn_cfg.mqtt_events_topic[sizeof(s_conn_cfg.mqtt_events_topic) - 1] = '\0';
    s_conn_cfg.mqtt_warnings_topic[sizeof(s_conn_cfg.mqtt_warnings_topic) - 1] = '\0';
    s_conn_cfg.mqtt_game_state_topic[sizeof(s_conn_cfg.mqtt_game_state_topic) - 1] = '\0';
    s_conn_cfg.mqtt_prop_state_topic[sizeof(s_conn_cfg.mqtt_prop_state_topic) - 1] = '\0';
    s_conn_cfg.network_name[sizeof(s_conn_cfg.network_name) - 1] = '\0';
    s_conn_cfg.ap_password[sizeof(s_conn_cfg.ap_password) - 1] = '\0';
    if (s_conn_cfg.mqtt_port < 1 || s_conn_cfg.mqtt_port > 65535) {
        s_conn_cfg.mqtt_port = 1883;
    }

    return ESP_OK;
}

static esp_err_t save_connection_cfg_nvs(void)
{
    nvs_handle_t nvs = 0;
    connection_store_t stored = {
        .version = 1,
        .cfg = s_conn_cfg,
    };

    esp_err_t err = nvs_open(CONN_STORE_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(nvs, CONN_STORE_KEY, &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
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

static bool json_extract_bool_local(const char *json, const char *key, bool *out)
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
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static void copy_bounded_local(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
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
    char ap_ip_text[32] = "192.168.4.1";
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            snprintf(ap_ip_text, sizeof(ap_ip_text), IPSTR, IP2STR(&ip_info.ip));
        }
    }

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
             "\"networkName\":\"%s\","
             "\"apPassword\":\"%s\","
             "\"apIpAddress\":\"%s\","
             "\"apEnabled\":%s"
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
             s_conn_cfg.network_name,
             s_conn_cfg.ap_password,
             ap_ip_text,
             s_conn_cfg.ap_enabled ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t connection_post_handler(httpd_req_t *req)
{
    char body[1024];
    char value[128];
    char new_wifi_ssid[sizeof(s_conn_cfg.wifi_ssid)];
    char new_wifi_password[sizeof(s_conn_cfg.wifi_password)];
    bool wifi_ssid_updated = false;
    bool wifi_password_updated = false;

    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    copy_bounded_local(new_wifi_ssid, sizeof(new_wifi_ssid), s_conn_cfg.wifi_ssid);
    copy_bounded_local(new_wifi_password, sizeof(new_wifi_password), s_conn_cfg.wifi_password);

    if (json_extract_string_local(body, "wifiSsid", value, sizeof(value))) {
        copy_bounded_local(new_wifi_ssid, sizeof(new_wifi_ssid), value);
        wifi_ssid_updated = true;
    }
    if (json_extract_string_local(body, "wifiPassword", value, sizeof(value))) {
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
            return httpd_resp_send(req, err_payload, HTTPD_RESP_USE_STRLEN);
        }

        copy_bounded_local(s_conn_cfg.wifi_ssid, sizeof(s_conn_cfg.wifi_ssid), new_wifi_ssid);
        copy_bounded_local(s_conn_cfg.wifi_password, sizeof(s_conn_cfg.wifi_password), new_wifi_password);
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
    if (json_extract_string_local(body, "apPassword", value, sizeof(value))) {
        copy_bounded_local(s_conn_cfg.ap_password, sizeof(s_conn_cfg.ap_password), value);
    }
    {
        bool ap_val;
        if (json_extract_bool_local(body, "apEnabled", &ap_val)) {
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
        return httpd_resp_send(req, err_payload, HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_sendstr(req,
                              connect_attempted
                                  ? "{\"ok\":true,\"applied\":true,\"connecting\":true}"
                                  : "{\"ok\":true,\"applied\":true,\"connecting\":false}");
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

    char payload[1024];
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
             "\"status\":\"%s\","
             "\"apIpAddress\":\"%s\","
             "\"wifiConnected\":%s,"
             "\"wifiConnecting\":%s,"
             "\"wifiTargetSsid\":\"%s\","
             "\"wifiSsid\":\"%s\","
             "\"wifiRssi\":%d,"
             "\"wifiLastError\":\"%s\","
             "\"wifiLastErrorCode\":%d,"
             "\"pendingApShutdown\":%s"
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
             game_state,
             ap_ip_text,
             wifi_connected ? "true" : "false",
             s_sta_connecting ? "true" : "false",
             s_conn_cfg.wifi_ssid,
             wifi_ssid_json,
             wifi_rssi,
             s_sta_last_error,
             s_sta_last_disconnect_reason,
             s_ap_shutdown_pending ? "true" : "false");

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

    /* Cancel any pending backoff reconnect */
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
            ESP_LOGI(TAG, "Loaded connection settings from NVS");
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
    config.stack_size = 8192;

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

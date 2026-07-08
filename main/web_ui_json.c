#include "web_ui_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_bounded(char *dst, size_t dst_size, const char *src)
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

static void json_add_string(cJSON *object, const char *key, const char *value)
{
    cJSON_AddStringToObject(object, key, value ? value : "");
}

static esp_err_t json_parse_object(const char *json, cJSON **out)
{
    cJSON *root;

    if (!json || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return ESP_ERR_INVALID_ARG;
    }

    *out = root;
    return ESP_OK;
}

static void json_merge_object(cJSON *dst, const cJSON *src)
{
    for (const cJSON *child = src ? src->child : NULL; child; child = child->next) {
        if (!child->string) {
            continue;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(dst, child->string);
        cJSON_AddItemToObject(dst, child->string, cJSON_Duplicate(child, true));
    }
}

cJSON *web_ui_json_parse(const char *json)
{
    cJSON *root = NULL;

    if (json_parse_object(json, &root) != ESP_OK) {
        return NULL;
    }

    return root;
}

bool web_ui_json_has_key(const cJSON *object, const char *key)
{
    return object && key && cJSON_GetObjectItemCaseSensitive((cJSON *)object, key) != NULL;
}

bool web_ui_json_get_string(const cJSON *object, const char *key, char *out, size_t out_size)
{
    const cJSON *item;

    if (!object || !key || !out || out_size == 0) {
        return false;
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)object, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }

    copy_bounded(out, out_size, item->valuestring);
    return true;
}

bool web_ui_json_get_int(const cJSON *object, const char *key, int *out)
{
    const cJSON *item;

    if (!object || !key || !out) {
        return false;
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)object, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }

    *out = item->valueint;
    return true;
}

bool web_ui_json_get_bool(const cJSON *object, const char *key, bool *out)
{
    const cJSON *item;

    if (!object || !key || !out) {
        return false;
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)object, key);
    if (!cJSON_IsBool(item)) {
        return false;
    }

    *out = cJSON_IsTrue(item);
    return true;
}

bool web_ui_json_extract_string(const char *json, const char *key, char *out, size_t out_size)
{
    cJSON *root = web_ui_json_parse(json);
    bool ok = web_ui_json_get_string(root, key, out, out_size);
    cJSON_Delete(root);
    return ok;
}

bool web_ui_json_extract_int(const char *json, const char *key, int *out)
{
    cJSON *root = web_ui_json_parse(json);
    bool ok = web_ui_json_get_int(root, key, out);
    cJSON_Delete(root);
    return ok;
}

bool web_ui_json_extract_bool(const char *json, const char *key, bool *out)
{
    cJSON *root = web_ui_json_parse(json);
    bool ok = web_ui_json_get_bool(root, key, out);
    cJSON_Delete(root);
    return ok;
}

esp_err_t web_ui_json_load_connection_cfg(const char *path, connection_cfg_t *cfg)
{
    FILE *f;
    long size;
    char *buf = NULL;
    cJSON *root = NULL;
    int i_val;
    bool b_val;

    if (!path || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    f = fopen(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    size = ftell(f);
    if (size <= 0 || size > 8192) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);

    buf = (char *)calloc(1, (size_t)size + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return ESP_FAIL;
    }
    fclose(f);

    if (json_parse_object(buf, &root) != ESP_OK) {
        free(buf);
        return ESP_ERR_INVALID_ARG;
    }

    (void)web_ui_json_get_string(root, "wifiSsid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    (void)web_ui_json_get_string(root, "wifiPassword", cfg->wifi_password, sizeof(cfg->wifi_password));
    (void)web_ui_json_get_string(root, "mqttHost", cfg->mqtt_host, sizeof(cfg->mqtt_host));
    if (web_ui_json_get_int(root, "mqttPort", &i_val) && i_val >= 1 && i_val <= 65535) {
        cfg->mqtt_port = i_val;
    }
    (void)web_ui_json_get_string(root, "mqttUsername", cfg->mqtt_username, sizeof(cfg->mqtt_username));
    (void)web_ui_json_get_string(root, "mqttPassword", cfg->mqtt_password, sizeof(cfg->mqtt_password));
    (void)web_ui_json_get_string(root, "mqttBaseTopic", cfg->mqtt_base_topic, sizeof(cfg->mqtt_base_topic));
    (void)web_ui_json_get_string(root,
                                 "mqttGameStateTopic",
                                 cfg->mqtt_game_state_topic,
                                 sizeof(cfg->mqtt_game_state_topic));
    if (!web_ui_json_get_string(root,
                                "mqttPropAnnounceTopic",
                                cfg->mqtt_prop_announce_topic,
                                sizeof(cfg->mqtt_prop_announce_topic))) {
        (void)web_ui_json_get_string(root,
                                     "mqttPropStateTopic",
                                     cfg->mqtt_prop_announce_topic,
                                     sizeof(cfg->mqtt_prop_announce_topic));
    }
    (void)web_ui_json_get_string(root, "networkName", cfg->network_name, sizeof(cfg->network_name));
    (void)web_ui_json_get_string(root, "apPassword", cfg->ap_password, sizeof(cfg->ap_password));
    if (web_ui_json_get_bool(root, "apEnabled", &b_val)) {
        cfg->ap_enabled = b_val;
    }

    cJSON_Delete(root);
    free(buf);
    return ESP_OK;
}

esp_err_t web_ui_json_save_connection_cfg(const char *path,
                                          const connection_cfg_t *cfg,
                                          const char *prop_cfg_json)
{
    FILE *f;
    cJSON *root = NULL;
    cJSON *prop_root = NULL;
    char *rendered = NULL;
    esp_err_t err;

    if (!path || !cfg || !prop_cfg_json) {
        return ESP_ERR_INVALID_ARG;
    }

    err = json_parse_object(prop_cfg_json, &prop_root);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(prop_root);
        return ESP_ERR_NO_MEM;
    }

    json_add_string(root, "wifiSsid", cfg->wifi_ssid);
    json_add_string(root, "wifiPassword", cfg->wifi_password);
    json_add_string(root, "mqttHost", cfg->mqtt_host);
    cJSON_AddNumberToObject(root, "mqttPort", cfg->mqtt_port);
    json_add_string(root, "mqttUsername", cfg->mqtt_username);
    json_add_string(root, "mqttPassword", cfg->mqtt_password);
    json_add_string(root, "mqttBaseTopic", cfg->mqtt_base_topic);
    json_add_string(root, "mqttGameStateTopic", cfg->mqtt_game_state_topic);
    json_add_string(root, "mqttPropAnnounceTopic", cfg->mqtt_prop_announce_topic);
    json_add_string(root, "networkName", cfg->network_name);
    json_add_string(root, "apPassword", cfg->ap_password);
    cJSON_AddBoolToObject(root, "apEnabled", cfg->ap_enabled);
    json_merge_object(root, prop_root);

    rendered = cJSON_Print(root);
    cJSON_Delete(prop_root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }

    f = fopen(path, "w");
    if (!f) {
        cJSON_free(rendered);
        return ESP_FAIL;
    }

    if (fputs(rendered, f) == EOF || fputc('\n', f) == EOF || fclose(f) != 0) {
        cJSON_free(rendered);
        return ESP_FAIL;
    }

    cJSON_free(rendered);
    return ESP_OK;
}

char *web_ui_json_build_unified_config_payload(const connection_cfg_t *cfg,
                                               const char *commands_topic,
                                               const char *state_topic,
                                               const char *events_topic,
                                               const char *warnings_topic,
                                               const char *prop_cfg_json)
{
    cJSON *root = NULL;
    cJSON *prop_root = NULL;
    char *payload = NULL;

    if (!cfg || !commands_topic || !state_topic || !events_topic || !warnings_topic || !prop_cfg_json) {
        return NULL;
    }

    if (json_parse_object(prop_cfg_json, &prop_root) != ESP_OK) {
        return NULL;
    }

    root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(prop_root);
        return NULL;
    }

    json_add_string(root, "wifiSsid", cfg->wifi_ssid);
    json_add_string(root, "wifiPassword", cfg->wifi_password);
    json_add_string(root, "mqttHost", cfg->mqtt_host);
    cJSON_AddNumberToObject(root, "mqttPort", cfg->mqtt_port);
    json_add_string(root, "mqttUsername", cfg->mqtt_username);
    json_add_string(root, "mqttPassword", cfg->mqtt_password);
    json_add_string(root, "mqttBaseTopic", cfg->mqtt_base_topic);
    json_add_string(root, "mqttCommandTopic", commands_topic);
    json_add_string(root, "mqttStateTopic", state_topic);
    json_add_string(root, "mqttEventsTopic", events_topic);
    json_add_string(root, "mqttWarningsTopic", warnings_topic);
    json_add_string(root, "mqttGameStateTopic", cfg->mqtt_game_state_topic);
    json_add_string(root, "mqttPropAnnounceTopic", cfg->mqtt_prop_announce_topic);
    json_add_string(root, "mqttPropStateTopic", cfg->mqtt_prop_announce_topic);
    json_add_string(root, "networkName", cfg->network_name);
    json_add_string(root, "apPassword", cfg->ap_password);
    cJSON_AddBoolToObject(root, "apEnabled", cfg->ap_enabled);
    json_merge_object(root, prop_root);

    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(prop_root);
    cJSON_Delete(root);
    return payload;
}

char *web_ui_json_build_connection_payload(const connection_cfg_t *cfg,
                                           const char *commands_topic,
                                           const char *state_topic,
                                           const char *events_topic,
                                           const char *warnings_topic,
                                           const char *ap_ssid,
                                           const char *ap_ip_address)
{
    cJSON *root;

    if (!cfg || !commands_topic || !state_topic || !events_topic || !warnings_topic) {
        return NULL;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    json_add_string(root, "wifiSsid", cfg->wifi_ssid);
    json_add_string(root, "wifiPassword", cfg->wifi_password);
    json_add_string(root, "mqttHost", cfg->mqtt_host);
    cJSON_AddNumberToObject(root, "mqttPort", cfg->mqtt_port);
    json_add_string(root, "mqttUsername", cfg->mqtt_username);
    json_add_string(root, "mqttPassword", cfg->mqtt_password);
    json_add_string(root, "mqttBaseTopic", cfg->mqtt_base_topic);
    json_add_string(root, "mqttCommandTopic", commands_topic);
    json_add_string(root, "mqttStateTopic", state_topic);
    json_add_string(root, "mqttEventsTopic", events_topic);
    json_add_string(root, "mqttWarningsTopic", warnings_topic);
    json_add_string(root, "mqttGameStateTopic", cfg->mqtt_game_state_topic);
    json_add_string(root, "mqttPropAnnounceTopic", cfg->mqtt_prop_announce_topic);
    json_add_string(root, "mqttPropStateTopic", cfg->mqtt_prop_announce_topic);
    json_add_string(root, "networkName", cfg->network_name);
    json_add_string(root, "apSsid", ap_ssid);
    json_add_string(root, "apPassword", cfg->ap_password);
    json_add_string(root, "apIpAddress", ap_ip_address);
    cJSON_AddBoolToObject(root, "apEnabled", cfg->ap_enabled);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

char *web_ui_json_build_device_details_payload(const char *prop_name,
                                               const char *ip_address,
                                               const char *software_version,
                                               const char *build_number,
                                               const char *build_date,
                                               const char *build_time,
                                               int64_t free_memory_bytes,
                                               int battery_percent,
                                               const char *battery_state,
                                               int battery_voltage_mv,
                                               bool battery_low,
                                               const char *network_name,
                                               const char *status,
                                               const char *ap_ip_address,
                                               bool wifi_connected,
                                               bool wifi_connecting,
                                               const char *wifi_target_ssid,
                                               const char *wifi_ssid,
                                               int wifi_rssi,
                                               const char *wifi_last_error,
                                               int wifi_last_error_code,
                                               bool pending_ap_shutdown)
{
    cJSON *root = cJSON_CreateObject();

    if (!root) {
        return NULL;
    }

    json_add_string(root, "propName", prop_name);
    json_add_string(root, "ipAddress", ip_address);
    json_add_string(root, "softwareVersion", software_version);
    json_add_string(root, "buildNumber", build_number);

    char build_date_time[96];
    snprintf(build_date_time, sizeof(build_date_time), "%s %s", build_date ? build_date : "", build_time ? build_time : "");
    json_add_string(root, "buildDate", build_date_time);
    cJSON_AddNullToObject(root, "cpuTempC");
    cJSON_AddNumberToObject(root, "freeMemoryBytes", (double)free_memory_bytes);
    cJSON_AddNumberToObject(root, "batteryPercent", battery_percent);
    json_add_string(root, "batteryState", battery_state && battery_state[0] ? battery_state : "normal");
    cJSON_AddNumberToObject(root, "batteryVoltageMv", battery_voltage_mv);
    cJSON_AddBoolToObject(root, "lowBattery", battery_low);
    json_add_string(root, "networkName", network_name);
    json_add_string(root, "status", status);
    json_add_string(root, "apIpAddress", ap_ip_address);
    cJSON_AddBoolToObject(root, "wifiConnected", wifi_connected);
    cJSON_AddBoolToObject(root, "wifiConnecting", wifi_connecting);
    json_add_string(root, "wifiTargetSsid", wifi_target_ssid);
    json_add_string(root, "wifiSsid", wifi_ssid);
    cJSON_AddNumberToObject(root, "wifiRssi", wifi_rssi);
    json_add_string(root, "wifiLastError", wifi_last_error);
    cJSON_AddNumberToObject(root, "wifiLastErrorCode", wifi_last_error_code);
    cJSON_AddBoolToObject(root, "pendingApShutdown", pending_ap_shutdown);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

char *web_ui_json_build_device_name_payload(const char *network_name)
{
    cJSON *root = cJSON_CreateObject();
    char url[96];

    if (!root) {
        return NULL;
    }

    snprintf(url, sizeof(url), "http://%s.local", network_name ? network_name : "");
    cJSON_AddBoolToObject(root, "ok", true);
    json_add_string(root, "networkName", network_name);
    json_add_string(root, "url", url);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

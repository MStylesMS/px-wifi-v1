#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "cJSON.h"

#define WEB_UI_CONFIG_FILE_PATH "/spiffs/config.json"

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char mqtt_host[128];
    int mqtt_port;
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_base_topic[96];
    char mqtt_game_state_topic[128];
    char mqtt_prop_announce_topic[128];
    char network_name[33];
    char ap_password[65];
    char ui_password[65];
    bool ap_enabled;
} connection_cfg_t;

#define WEB_UI_CONNECTION_CFG_DEFAULT { \
    .wifi_ssid = "", \
    .wifi_password = "", \
    .mqtt_host = "", \
    .mqtt_port = 1883, \
    .mqtt_username = "", \
    .mqtt_password = "", \
    .mqtt_base_topic = "site/room/zone", \
    .mqtt_game_state_topic = "site/room/state", \
    .mqtt_prop_announce_topic = "site/props", \
    .network_name = "", \
    .ap_password = "", \
    .ui_password = "", \
    .ap_enabled = true, \
}

cJSON *web_ui_json_parse(const char *json);
bool web_ui_json_has_key(const cJSON *object, const char *key);
bool web_ui_json_get_string(const cJSON *object, const char *key, char *out, size_t out_size);
bool web_ui_json_get_int(const cJSON *object, const char *key, int *out);
bool web_ui_json_get_bool(const cJSON *object, const char *key, bool *out);

bool web_ui_json_extract_string(const char *json, const char *key, char *out, size_t out_size);
bool web_ui_json_extract_int(const char *json, const char *key, int *out);
bool web_ui_json_extract_bool(const char *json, const char *key, bool *out);

esp_err_t web_ui_json_load_connection_cfg(const char *path, connection_cfg_t *cfg);
esp_err_t web_ui_json_save_connection_cfg(const char *path,
                                          const connection_cfg_t *cfg,
                                          const char *prop_cfg_json);

char *web_ui_json_build_unified_config_payload(const connection_cfg_t *cfg,
                                               const char *commands_topic,
                                               const char *state_topic,
                                               const char *events_topic,
                                               const char *warnings_topic,
                                               const char *prop_cfg_json);

char *web_ui_json_build_connection_payload(const connection_cfg_t *cfg,
                                           const char *commands_topic,
                                           const char *state_topic,
                                           const char *events_topic,
                                           const char *warnings_topic,
                                           const char *ap_ssid,
                                           const char *ap_ip_address);

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
                                               bool has_cpu_temp,
                                               float cpu_temp_c,
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
                                               bool pending_ap_shutdown);

char *web_ui_json_build_device_name_payload(const char *network_name);

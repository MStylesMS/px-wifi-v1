#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    PROP_STATE_NOT_READY = 0,
    PROP_STATE_READY,
    PROP_STATE_COUNTDOWN,
    PROP_STATE_PAUSED,
    PROP_STATE_DEFUSED,
    PROP_STATE_DETONATED,
} prop_state_t;

typedef enum {
    PROP_LED_HINT_OFF = 0,
    PROP_LED_HINT_AP_MODE,
    PROP_LED_HINT_CONNECTING_WIFI,
    PROP_LED_HINT_CONNECTING_MQTT,
    PROP_LED_HINT_READY,
    PROP_LED_HINT_NOT_READY,
    PROP_LED_HINT_COUNTDOWN,
    PROP_LED_HINT_PAUSED,
    PROP_LED_HINT_PENALTY,
    PROP_LED_HINT_DETONATED,
    PROP_LED_HINT_DEFUSED,
    PROP_LED_HINT_OTA,
} prop_led_hint_t;

#define PROP_BUZZER_MML_MAX_LEN 192

typedef struct {
    int default_time_s;
    int penalty_s;
    int max_tries;
    int wire_count;
    int debounce_check_interval_ms;
    int debounce_consecutive_reads;
    int dedupe_window_ms;
    int hold_result_s;
    int heartbeat_interval_s;
    int keep_sync_max_drift_ms;
    int led_brightness_percent;
    int wifi_ap_timeout_s;
    int deep_sleep_window_s;
    int max_idle_cycle_s;
    int time_tolerance_ms;
    bool keep_sync_enabled;
    bool lid_enabled;
    bool lid_normally_closed;
    char mode[16];
    char lid_mode[20];
    char solution[9];
    char input_names[8][16];
    char buzzer_start_resume_mml[PROP_BUZZER_MML_MAX_LEN];
    char buzzer_pause_reset_mml[PROP_BUZZER_MML_MAX_LEN];
    char buzzer_solved_mml[PROP_BUZZER_MML_MAX_LEN];
    char buzzer_failed_mml[PROP_BUZZER_MML_MAX_LEN];
} prop_config_t;

typedef struct {
    char start_resume[PROP_BUZZER_MML_MAX_LEN];
    char pause_reset[PROP_BUZZER_MML_MAX_LEN];
    char solved[PROP_BUZZER_MML_MAX_LEN];
    char failed[PROP_BUZZER_MML_MAX_LEN];
} prop_buzzer_mml_config_t;

esp_err_t prop_engine_init(void);

void prop_engine_get_state_json(char *out, size_t out_size);
void prop_engine_get_config_json(char *out, size_t out_size);
void prop_engine_get_default_config_json(char *out, size_t out_size);

esp_err_t prop_engine_handle_command_json(const char *json, char *response, size_t response_size);
esp_err_t prop_engine_apply_config_json(const char *json, bool persist, char *response, size_t response_size);
esp_err_t prop_engine_restore_defaults(bool persist, char *response, size_t response_size);

prop_led_hint_t prop_engine_get_led_hint(void);
void prop_engine_get_buzzer_mml_config(prop_buzzer_mml_config_t *out);

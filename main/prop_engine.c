#include "prop_engine.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "prop_engine";

/* GPIO mapping from docs/pin-mapping.md for DevKitC-1 v1.0 */
static const gpio_num_t s_wire_input_gpios[8] = {
    GPIO_NUM_4,   /* INPUT_1 red */
    GPIO_NUM_5,   /* INPUT_2 green */
    GPIO_NUM_6,   /* INPUT_3 yellow */
    GPIO_NUM_7,   /* INPUT_4 blue */
    GPIO_NUM_15,  /* INPUT_5 aux_1 */
    GPIO_NUM_16,  /* INPUT_6 aux_2 */
    GPIO_NUM_17,  /* INPUT_7 aux_3 */
    GPIO_NUM_18,  /* INPUT_8 lid_switch */
};

/* GPIO8 is reserved as the common low-side return for wire harness inputs. */
static const gpio_num_t s_wire_ground_gpio = GPIO_NUM_8;

#define BATTERY_MAX_POINTS 20
#define BATTERY_FILE_PATH "/spiffs/battery_profile.json"
#define BATTERY_EXTERNAL_THRESHOLD_MV 5000
#define BATTERY_ADC_MAX_VALUE 4095
#define BATTERY_DIVIDER_R1_OHMS 21600
#define BATTERY_DIVIDER_R2_OHMS 4430
#define BATTERY_ADC_FULL_SCALE_MV 15000
#define LOW_BATTERY_CUTOFF_DELAY_MS 15000
#define DEEP_SLEEP_WAKE_GPIO GPIO_NUM_4
#define RESULT_RESET_DELAY_S 60
#define PROP_EVENT_QUEUE_LEN 8
#define PROP_EVENT_NAME_LEN 24

typedef struct {
    int mv;
    int pct;
} battery_point_t;

typedef struct {
    char name[PROP_EVENT_NAME_LEN];
    char mode[16];
    prop_state_t state;
    int time_remaining_ms;
    int tries_used;
    int max_tries;
    int64_t ts_ms;
} prop_event_t;

typedef struct {
    const char *name;
    int count;
    battery_point_t points[BATTERY_MAX_POINTS];
} battery_profile_builtin_t;

static const battery_profile_builtin_t s_builtin_profiles[] = {
    {
        .name = "6v-lead-acid",
        .count = 8,
        .points = {
            {6600, 100}, {6450, 95}, {6350, 85}, {6250, 70},
            {6150, 55}, {6050, 35}, {5950, 15}, {5850, 0},
        },
    },
    {
        .name = "6v-LiFePO4",
        .count = 8,
        .points = {
            {7400, 100}, {7200, 96}, {6900, 88}, {6700, 70},
            {6550, 52}, {6400, 30}, {6250, 12}, {6100, 0},
        },
    },
    {
        .name = "12v-lead-acid",
        .count = 8,
        .points = {
            {13200, 100}, {12900, 95}, {12700, 84}, {12500, 70},
            {12300, 55}, {12100, 35}, {11900, 15}, {11700, 0},
        },
    },
    {
        .name = "12v-LiFePO4",
        .count = 8,
        .points = {
            {14600, 100}, {14200, 96}, {13800, 88}, {13400, 70},
            {13100, 52}, {12800, 30}, {12500, 12}, {12200, 0},
        },
    },
    {
        .name = "external",
        .count = 2,
        .points = {
            {5000, 100}, {0, 100},
        },
    },
    {
        .name = "unknown",
        .count = 2,
        .points = {
            {5000, 100}, {0, 100},
        },
    },
};

typedef struct {
    prop_config_t cfg;
    prop_state_t state;
    int time_remaining_ms;
    int tries_used;
    bool ready_show_time;
    bool stopped;
    uint8_t connected_mask;
    char disconnected_order[9];
    char last_cmd[128];
    int64_t last_cmd_ms;
    int64_t state_enter_ms;
    int64_t penalty_until_ms;

    char battery_profile[24];
    int low_battery_percent;
    int low_battery_cutoff_percent;
    int battery_shutdown_delay_s;
    int battery_adc_raw;
    int battery_adc_at_0v;
    int battery_adc_at_15v;
    int battery_voltage_mv;
    int battery_percent;
    bool battery_low;
    int64_t battery_zero_since_ms;
    int64_t battery_cutoff_since_ms;
    battery_point_t battery_points[BATTERY_MAX_POINTS];
    int battery_point_count;
    prop_event_t event_queue[PROP_EVENT_QUEUE_LEN];
    int event_head;
    int event_tail;

    bool spiffs_ready;
    SemaphoreHandle_t lock;
} prop_ctx_t;

static prop_ctx_t s_ctx;

static void prop_engine_get_state_json_unlocked(char *out, size_t out_size);
static void handle_disconnect_unlocked(int idx);
static void handle_connect_unlocked(int idx);
static void queue_event_unlocked(const char *event_name);
static const char *state_name(prop_state_t state);
static bool json_extract_string(const char *json, const char *key, char *out, size_t out_size);
static bool json_extract_int(const char *json, const char *key, int *out);

static void configure_deep_sleep_wake_gpio(void)
{
    gpio_num_t wake_gpio = DEEP_SLEEP_WAKE_GPIO;
    int level = gpio_get_level(wake_gpio);
    esp_sleep_ext1_wakeup_mode_t mode =
        (level == 0) ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ANY_LOW;
    esp_err_t err;

    /* Ensure wake source selection starts from a known state. */
    err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear wake sources: %s", esp_err_to_name(err));
    }

    err = esp_sleep_enable_ext1_wakeup_io((1ULL << wake_gpio), mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to configure deep sleep wake on GPIO %d: %s",
                 wake_gpio,
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG,
                 "Deep sleep wake armed on GPIO %d (current=%d, mode=%s)",
                 wake_gpio,
                 level,
                 (mode == ESP_EXT1_WAKEUP_ANY_HIGH) ? "ANY_HIGH" : "ANY_LOW");
    }
}

static esp_err_t init_wire_inputs(void)
{
    esp_err_t err;

    /* Reset pins from any default IOMUX functions first */
    for (int i = 0; i < 8; ++i) {
        err = gpio_reset_pin(s_wire_input_gpios[i]);
        ESP_LOGI(TAG, "gpio_reset_pin(%d) = %s", s_wire_input_gpios[i], esp_err_to_name(err));
    }

    /* Configure each pin individually so we can log per-pin results */
    for (int i = 0; i < 8; ++i) {
        gpio_config_t io_cfg = {
            .pin_bit_mask = (1ULL << s_wire_input_gpios[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&io_cfg);
        int level = gpio_get_level(s_wire_input_gpios[i]);
        ESP_LOGI(TAG, "gpio_config GPIO %d: %s, level=%d", s_wire_input_gpios[i], esp_err_to_name(err), level);
    }

    /* Test: flip to pull-down to prove config takes effect */
    for (int i = 0; i < 4; ++i) {
        gpio_set_pull_mode(s_wire_input_gpios[i], GPIO_PULLDOWN_ONLY);
        vTaskDelay(pdMS_TO_TICKS(5));
        int level_pd = gpio_get_level(s_wire_input_gpios[i]);
        /* Restore pull-up */
        gpio_set_pull_mode(s_wire_input_gpios[i], GPIO_PULLUP_ONLY);
        vTaskDelay(pdMS_TO_TICKS(5));
        int level_pu = gpio_get_level(s_wire_input_gpios[i]);
        ESP_LOGI(TAG, "GPIO %d: pull-down→%d, pull-up→%d", s_wire_input_gpios[i], level_pd, level_pu);
    }

    /* Test: drive GPIO 4-7 as outputs LOW, read back, then restore to input */
    ESP_LOGW(TAG, "=== OUTPUT DRIVE TEST (driving LOW) ===");
    for (int i = 0; i < 4; ++i) {
        gpio_num_t pin = s_wire_input_gpios[i];
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        /* Read raw register to see if output took effect */
        uint32_t gpio_in = REG_READ(GPIO_IN_REG);
        int bit = (gpio_in >> pin) & 1;
        ESP_LOGW(TAG, "GPIO %d: output LOW → raw bit=%d", pin, bit);
        /* Now drive HIGH */
        gpio_set_level(pin, 1);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_in = REG_READ(GPIO_IN_REG);
        bit = (gpio_in >> pin) & 1;
        ESP_LOGW(TAG, "GPIO %d: output HIGH → raw bit=%d", pin, bit);
        /* Restore to input with pull-up */
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    }

    /* Read IO_MUX register for GPIO 5,6,7 to check routing */
    ESP_LOGW(TAG, "=== IO_MUX REGISTER DUMP ===");
    for (int i = 0; i < 4; ++i) {
        gpio_num_t pin = s_wire_input_gpios[i];
        /* IO_MUX registers are at IO_MUX_GPIO0_REG + pin*4 */
        uint32_t iomux_reg = REG_READ(IO_MUX_GPIO0_REG + pin * 4);
        uint32_t gpio_func = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + pin * 4);
        ESP_LOGW(TAG, "GPIO %d: IO_MUX=0x%08lx, GPIO_FUNCx_OUT_SEL=0x%08lx",
                 pin, (unsigned long)iomux_reg, (unsigned long)gpio_func);
    }

    return ESP_OK;
}

static esp_err_t init_wire_ground_drive(void)
{
    esp_err_t err;

    err = gpio_reset_pin(s_wire_ground_gpio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_reset_pin(%d) failed: %s", s_wire_ground_gpio, esp_err_to_name(err));
        return err;
    }

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << s_wire_ground_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(%d) failed: %s", s_wire_ground_gpio, esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(s_wire_ground_gpio, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level(%d,0) failed: %s", s_wire_ground_gpio, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "GPIO %d configured as wire ground drive (OUTPUT LOW)", s_wire_ground_gpio);
    return ESP_OK;
}

static void wire_input_task(void *arg)
{
    int stable_count[8] = {0};
    bool last_sample_connected[8] = {0};
    bool debounced_connected[8] = {0};

    for (int i = 0; i < 8; ++i) {
        int level = gpio_get_level(s_wire_input_gpios[i]);
        bool connected = (level == 0);
        last_sample_connected[i] = connected;
        debounced_connected[i] = connected;
        stable_count[i] = 1;
    }

    while (true) {
        int check_interval_ms;
        int required_consecutive;

        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        check_interval_ms = s_ctx.cfg.debounce_check_interval_ms;
        required_consecutive = s_ctx.cfg.debounce_consecutive_reads;
        xSemaphoreGive(s_ctx.lock);

        if (check_interval_ms < 1) {
            check_interval_ms = 10;
        }
        if (required_consecutive < 1) {
            required_consecutive = 1;
        }

        vTaskDelay(pdMS_TO_TICKS(check_interval_ms));

        for (int i = 0; i < 8; ++i) {
            int level = gpio_get_level(s_wire_input_gpios[i]);
            bool connected = (level == 0); /* active-low input */

            if (connected == last_sample_connected[i]) {
                if (stable_count[i] < required_consecutive) {
                    stable_count[i]++;
                }
            } else {
                last_sample_connected[i] = connected;
                stable_count[i] = 1;
            }

            if (stable_count[i] >= required_consecutive && connected != debounced_connected[i]) {
                debounced_connected[i] = connected;

                ESP_LOGW(TAG, "GPIO %d (input %d): %s → %s",
                         s_wire_input_gpios[i], i + 1,
                         connected ? "HIGH" : "LOW",
                         connected ? "LOW (connected)" : "HIGH (disconnected)");

                xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
                if (connected) {
                    handle_connect_unlocked(i + 1);
                } else {
                    handle_disconnect_unlocked(i + 1);
                }
                xSemaphoreGive(s_ctx.lock);
            }
        }
    }
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int clamp_int(int value, int min_v, int max_v)
{
    if (value < min_v) {
        return min_v;
    }
    if (value > max_v) {
        return max_v;
    }
    return value;
}

static void copy_bounded(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static int heartbeat_ms_from_cfg(const prop_config_t *cfg)
{
    return cfg->heartbeat_interval_s * 1000;
}

static bool parse_time_text_to_seconds(const char *text, int *out_seconds)
{
    int hh = 0;
    int mm = 0;
    int ss = 0;
    char *endptr = NULL;
    long value;

    if (!text || !text[0] || !out_seconds) {
        return false;
    }

    if (sscanf(text, "%d:%d:%d", &hh, &mm, &ss) == 3) {
        if (hh < 0 || mm < 0 || ss < 0) {
            return false;
        }
        *out_seconds = (hh * 3600) + (mm * 60) + ss;
        return true;
    }

    if (sscanf(text, "%d:%d", &mm, &ss) == 2) {
        if (mm < 0 || ss < 0) {
            return false;
        }
        *out_seconds = (mm * 60) + ss;
        return true;
    }

    value = strtol(text, &endptr, 10);
    if (!endptr || *endptr != '\0' || value < 0 || value > 24L * 3600L) {
        return false;
    }

    *out_seconds = (int)value;
    return true;
}

static bool json_extract_time_seconds(const char *json, const char *key, int *out_seconds)
{
    int int_value;
    char text_value[32];

    if (json_extract_string(json, key, text_value, sizeof(text_value))) {
        return parse_time_text_to_seconds(text_value, out_seconds);
    }

    if (json_extract_int(json, key, &int_value) && int_value >= 0) {
        *out_seconds = int_value;
        return true;
    }

    return false;
}

static void build_disconnected_inputs_list(char *out, size_t out_size)
{
    uint8_t required = (uint8_t)((1u << s_ctx.cfg.wire_count) - 1u);
    uint8_t missing = (uint8_t)(required & ~s_ctx.connected_mask);
    size_t pos = 0;

    if (!out || out_size == 0) {
        return;
    }

    out[0] = '\0';
    for (int bit = 0; bit < s_ctx.cfg.wire_count; ++bit) {
        const char *name;

        if ((missing & (1u << bit)) == 0) {
            continue;
        }

        if (pos > 0 && pos < out_size - 1) {
            out[pos++] = ',';
        }
        if (pos > 0 && pos < out_size - 1) {
            out[pos++] = ' ';
        }

        name = s_ctx.cfg.input_names[bit];
        while (*name && pos < out_size - 1) {
            out[pos++] = *name++;
        }
    }
    out[pos] = '\0';
}

static void build_not_ready_response(const char *event_name,
                                     const char *message,
                                     bool ok,
                                     char *response,
                                     size_t response_size)
{
    char disc_list[160];

    build_disconnected_inputs_list(disc_list, sizeof(disc_list));
    if (disc_list[0] != '\0') {
        snprintf(response,
                 response_size,
                 "{\"ok\":%s,\"error\":\"notReady\",\"event\":\"%s\","
                 "\"state\":\"%s\",\"message\":\"%s\","
                 "\"details\":\"Inputs not closed: %s\","
                 "\"disconnected\":\"%s\"}",
                 ok ? "true" : "false",
                 event_name,
                 state_name(s_ctx.state),
                 message,
                 disc_list,
                 disc_list);
        return;
    }

    snprintf(response,
             response_size,
             "{\"ok\":%s,\"error\":\"notReady\",\"event\":\"%s\","
             "\"state\":\"%s\",\"message\":\"%s\"}",
             ok ? "true" : "false",
             event_name,
             state_name(s_ctx.state),
             message);
}

static int solution_length_unlocked(void)
{
    return (int)strlen(s_ctx.cfg.solution);
}

static bool sanitize_solution_vector(const char *src, char *out, size_t out_size)
{
    size_t i;
    size_t w = 0;

    if (!src || !out || out_size < 2) {
        return false;
    }

    for (i = 0; src[i] != '\0' && w < out_size - 1; ++i) {
        unsigned char ch = (unsigned char)src[i];
        if (isspace(ch)) {
            continue;
        }
        if (ch >= '1' && ch <= '8') {
            out[w++] = (char)ch;
        }
    }

    out[w] = '\0';
    return w > 0;
}

static int battery_divider_sense_mv_from_input_mv(int input_mv)
{
    int64_t num = (int64_t)input_mv * BATTERY_DIVIDER_R2_OHMS;
    int denom = BATTERY_DIVIDER_R1_OHMS + BATTERY_DIVIDER_R2_OHMS;

    return (int)(num / denom);
}

static int battery_divider_input_mv_from_sense_mv(int sense_mv)
{
    int64_t num = (int64_t)sense_mv * (BATTERY_DIVIDER_R1_OHMS + BATTERY_DIVIDER_R2_OHMS);

    return (int)(num / BATTERY_DIVIDER_R2_OHMS);
}

static int battery_voltage_mv_from_adc_raw(int adc_raw)
{
    int denom = s_ctx.battery_adc_at_15v - s_ctx.battery_adc_at_0v;
    int sense_ref_mv = battery_divider_sense_mv_from_input_mv(BATTERY_ADC_FULL_SCALE_MV);
    int64_t sense_mv;

    if (denom <= 0) {
        return clamp_int(s_ctx.battery_voltage_mv, 0, BATTERY_ADC_FULL_SCALE_MV);
    }

    sense_mv = ((int64_t)(adc_raw - s_ctx.battery_adc_at_0v) * sense_ref_mv) / denom;
    return clamp_int(battery_divider_input_mv_from_sense_mv((int)sense_mv), 0, BATTERY_ADC_FULL_SCALE_MV);
}

static int battery_adc_raw_from_voltage_mv(int mv)
{
    int sense_ref_mv = battery_divider_sense_mv_from_input_mv(BATTERY_ADC_FULL_SCALE_MV);
    int sense_mv = battery_divider_sense_mv_from_input_mv(clamp_int(mv, 0, BATTERY_ADC_FULL_SCALE_MV));
    int64_t num;

    if (s_ctx.battery_adc_at_15v <= s_ctx.battery_adc_at_0v) {
        return clamp_int(s_ctx.battery_adc_raw, 0, BATTERY_ADC_MAX_VALUE);
    }

    num = (int64_t)sense_mv * (s_ctx.battery_adc_at_15v - s_ctx.battery_adc_at_0v);
    return clamp_int(s_ctx.battery_adc_at_0v + (int)(num / sense_ref_mv), 0, BATTERY_ADC_MAX_VALUE);
}

static const battery_profile_builtin_t *find_builtin_profile(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(s_builtin_profiles) / sizeof(s_builtin_profiles[0]); ++i) {
        if (strcmp(name, s_builtin_profiles[i].name) == 0) {
            return &s_builtin_profiles[i];
        }
    }

    return NULL;
}

static void sort_battery_points_desc(battery_point_t *pts, int count)
{
    int i;

    for (i = 0; i < count - 1; ++i) {
        int j;
        for (j = i + 1; j < count; ++j) {
            if (pts[j].mv > pts[i].mv) {
                battery_point_t tmp = pts[i];
                pts[i] = pts[j];
                pts[j] = tmp;
            }
        }
    }
}

static void load_profile_points(const battery_profile_builtin_t *profile)
{
    int i;

    if (!profile) {
        return;
    }

    s_ctx.battery_point_count = clamp_int(profile->count, 2, BATTERY_MAX_POINTS);
    for (i = 0; i < s_ctx.battery_point_count; ++i) {
        s_ctx.battery_points[i] = profile->points[i];
    }
    sort_battery_points_desc(s_ctx.battery_points, s_ctx.battery_point_count);
}

static bool parse_points_csv(const char *csv, battery_point_t *pts, int *count_out)
{
    char buf[640];
    char *token;
    char *saveptr = NULL;
    int count = 0;

    if (!csv || !pts || !count_out) {
        return false;
    }

    copy_bounded(buf, sizeof(buf), csv);
    token = strtok_r(buf, ",", &saveptr);

    while (token && count < BATTERY_MAX_POINTS) {
        float v = 0.0f;
        int pct = 0;
        int scanned = sscanf(token, " %f : %d", &v, &pct);
        if (scanned == 2) {
            int mv = (int)(v * 1000.0f + 0.5f);
            pts[count].mv = clamp_int(mv, 1000, 20000);
            pts[count].pct = clamp_int(pct, 0, 100);
            count++;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }

    if (count < 2) {
        return false;
    }

    sort_battery_points_desc(pts, count);
    *count_out = count;
    return true;
}

static void format_points_csv(char *out, size_t out_size)
{
    size_t pos = 0;
    int i;

    if (out_size == 0) {
        return;
    }
    out[0] = '\0';

    for (i = 0; i < s_ctx.battery_point_count; ++i) {
        int w = snprintf(out + pos,
                         out_size - pos,
                         "%s%.3f:%d",
                         i == 0 ? "" : ",",
                         (double)s_ctx.battery_points[i].mv / 1000.0,
                         s_ctx.battery_points[i].pct);
        if (w < 0 || (size_t)w >= out_size - pos) {
            break;
        }
        pos += (size_t)w;
    }
}

static int battery_percent_from_voltage_mv(int mv)
{
    int i;

    if (s_ctx.battery_point_count < 2) {
        return 100;
    }

    if (mv >= s_ctx.battery_points[0].mv) {
        return s_ctx.battery_points[0].pct;
    }
    if (mv <= s_ctx.battery_points[s_ctx.battery_point_count - 1].mv) {
        return s_ctx.battery_points[s_ctx.battery_point_count - 1].pct;
    }

    for (i = 0; i < s_ctx.battery_point_count - 1; ++i) {
        battery_point_t hi = s_ctx.battery_points[i];
        battery_point_t lo = s_ctx.battery_points[i + 1];
        if (mv <= hi.mv && mv >= lo.mv) {
            int dv = hi.mv - lo.mv;
            int dp = hi.pct - lo.pct;
            if (dv <= 0) {
                return clamp_int(hi.pct, 0, 100);
            }
            return clamp_int(lo.pct + (dp * (mv - lo.mv)) / dv, 0, 100);
        }
    }

    return clamp_int(s_ctx.battery_points[s_ctx.battery_point_count - 1].pct, 0, 100);
}

static bool battery_profile_is_external_like(const char *profile)
{
    if (!profile) {
        return false;
    }

    return strcmp(profile, "external") == 0 || strcmp(profile, "unknown") == 0;
}

static void update_battery_runtime_unlocked(void)
{
    s_ctx.battery_voltage_mv = battery_voltage_mv_from_adc_raw(s_ctx.battery_adc_raw);

    if (battery_profile_is_external_like(s_ctx.battery_profile)) {
        s_ctx.battery_percent = 100;
        s_ctx.battery_low = s_ctx.battery_voltage_mv < BATTERY_EXTERNAL_THRESHOLD_MV;
        return;
    }

    s_ctx.battery_percent = battery_percent_from_voltage_mv(s_ctx.battery_voltage_mv);
    s_ctx.battery_low = s_ctx.battery_percent <= s_ctx.low_battery_percent;
}

static void set_default_battery_config(void)
{
    const battery_profile_builtin_t *profile = find_builtin_profile("unknown");

    copy_bounded(s_ctx.battery_profile, sizeof(s_ctx.battery_profile), "unknown");
    s_ctx.low_battery_percent = 40;
    s_ctx.low_battery_cutoff_percent = 20;
    s_ctx.battery_shutdown_delay_s = 60;
    s_ctx.battery_adc_at_0v = 0;
    s_ctx.battery_adc_at_15v = BATTERY_ADC_MAX_VALUE;
    s_ctx.battery_adc_raw = 1420;

    load_profile_points(profile);
    s_ctx.battery_voltage_mv = 5200;
    s_ctx.battery_adc_raw = battery_adc_raw_from_voltage_mv(s_ctx.battery_voltage_mv);
    s_ctx.battery_zero_since_ms = 0;
    s_ctx.battery_cutoff_since_ms = 0;
    update_battery_runtime_unlocked();
}

static void set_default_config(prop_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->default_time_s = 3600;
    cfg->penalty_s = 30;
    cfg->max_tries = 3;
    cfg->wire_count = 4;
    cfg->debounce_check_interval_ms = 10;
    cfg->debounce_consecutive_reads = 5;
    cfg->dedupe_window_ms = 750;
    cfg->hold_result_s = RESULT_RESET_DELAY_S;
    cfg->heartbeat_interval_s = 10;
    cfg->keep_sync_max_drift_ms = 1000;
    cfg->led_brightness_percent = 20;
    cfg->wifi_ap_timeout_s = 30;
    cfg->deep_sleep_window_s = 30;
    cfg->max_idle_cycle_s = 300;
    cfg->time_tolerance_ms = 1000;
    cfg->keep_sync_enabled = false;
    cfg->lid_enabled = false;
    cfg->lid_normally_closed = false;
    snprintf(cfg->mode, sizeof(cfg->mode), "instant");
    snprintf(cfg->lid_mode, sizeof(cfg->lid_mode), "off");
    snprintf(cfg->solution, sizeof(cfg->solution), "1234");

    snprintf(cfg->input_names[0], sizeof(cfg->input_names[0]), "red");
    snprintf(cfg->input_names[1], sizeof(cfg->input_names[1]), "green");
    snprintf(cfg->input_names[2], sizeof(cfg->input_names[2]), "yellow");
    snprintf(cfg->input_names[3], sizeof(cfg->input_names[3]), "blue");
    snprintf(cfg->input_names[4], sizeof(cfg->input_names[4]), "white");
    snprintf(cfg->input_names[5], sizeof(cfg->input_names[5]), "orange");
    snprintf(cfg->input_names[6], sizeof(cfg->input_names[6]), "brown");
    snprintf(cfg->input_names[7], sizeof(cfg->input_names[7]), "purple");

    /* Old start tune: "T168 O5 L16 V70 A R D" */
    snprintf(cfg->buzzer_start_resume_mml,
             sizeof(cfg->buzzer_start_resume_mml),
             "T200 O6 L32 V80 C R C");
    /* Old pause tune: "T156 O5 L16 V65 D R G4" */
    snprintf(cfg->buzzer_pause_reset_mml,
             sizeof(cfg->buzzer_pause_reset_mml),
             "T200 O5 L32 V80 C R C");
    snprintf(cfg->buzzer_solved_mml,
             sizeof(cfg->buzzer_solved_mml),
             "T184 O5 L16 V78 C E G R C6 R C6 E6 G6 L8 C7");
    snprintf(cfg->buzzer_failed_mml,
             sizeof(cfg->buzzer_failed_mml),
             "T108 O5 L16 V72 G F E R B4 R L8 G4");
}

static bool all_wires_connected(void)
{
    uint8_t required_mask = (uint8_t)((1u << s_ctx.cfg.wire_count) - 1u);
    return (s_ctx.connected_mask & required_mask) == required_mask;
}

static const char *state_name(prop_state_t state)
{
    switch (state) {
        case PROP_STATE_NOT_READY:
            return "not_ready";
        case PROP_STATE_READY:
            return "ready";
        case PROP_STATE_COUNTDOWN:
            return "countdown";
        case PROP_STATE_PAUSED:
            return "paused";
        case PROP_STATE_DEFUSED:
            return "defused";
        case PROP_STATE_DETONATED:
            return "detonated";
        default:
            return "unknown";
    }
}

static const char *led_hint_name(prop_led_hint_t hint)
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

static prop_led_hint_t led_hint_unlocked(void)
{
    if (now_ms() < s_ctx.penalty_until_ms) {
        return PROP_LED_HINT_PENALTY;
    }

    switch (s_ctx.state) {
        case PROP_STATE_READY:
            return PROP_LED_HINT_READY;
        case PROP_STATE_NOT_READY:
            return PROP_LED_HINT_NOT_READY;
        case PROP_STATE_COUNTDOWN:
            return PROP_LED_HINT_COUNTDOWN;
        case PROP_STATE_PAUSED:
            return PROP_LED_HINT_PAUSED;
        case PROP_STATE_DEFUSED:
            return PROP_LED_HINT_DEFUSED;
        case PROP_STATE_DETONATED:
            return PROP_LED_HINT_DETONATED;
        default:
            return PROP_LED_HINT_OFF;
    }
}

static void set_ready_state(void)
{
    prop_state_t prev = s_ctx.state;

    if (all_wires_connected()) {
        s_ctx.state = PROP_STATE_READY;
    } else {
        s_ctx.state = PROP_STATE_NOT_READY;
    }

    if (prev != s_ctx.state) {
        ESP_LOGI(TAG, "State transition: %s -> %s", state_name(prev), state_name(s_ctx.state));
    }
}

static void reset_round(void)
{
    s_ctx.time_remaining_ms = s_ctx.cfg.default_time_s * 1000;
    s_ctx.tries_used = 0;
    s_ctx.ready_show_time = false;
    s_ctx.stopped = false;
    s_ctx.penalty_until_ms = 0;
    s_ctx.disconnected_order[0] = '\0';
    set_ready_state();
}

static void queue_event_unlocked(const char *event_name)
{
    int next_tail;
    prop_event_t *slot;

    if (!event_name || !event_name[0]) {
        return;
    }

    next_tail = (s_ctx.event_tail + 1) % PROP_EVENT_QUEUE_LEN;
    if (next_tail == s_ctx.event_head) {
        s_ctx.event_head = (s_ctx.event_head + 1) % PROP_EVENT_QUEUE_LEN;
        ESP_LOGW(TAG, "Event queue full, dropping oldest event");
    }

    slot = &s_ctx.event_queue[s_ctx.event_tail];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->name, sizeof(slot->name), "%s", event_name);
    snprintf(slot->mode, sizeof(slot->mode), "%s", s_ctx.cfg.mode);
    slot->state = s_ctx.state;
    slot->time_remaining_ms = s_ctx.time_remaining_ms;
    slot->tries_used = s_ctx.tries_used;
    slot->max_tries = s_ctx.cfg.max_tries;
    slot->ts_ms = now_ms();
    s_ctx.event_tail = next_tail;
}

static bool is_active_state(void)
{
    return s_ctx.state == PROP_STATE_COUNTDOWN || s_ctx.state == PROP_STATE_PAUSED;
}

static bool json_extract_string(const char *json, const char *key, char *out, size_t out_size)
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
    while (*p && isspace((unsigned char)*p)) {
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

static bool json_extract_int(const char *json, const char *key, int *out)
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
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    *out = (int)strtol(p, NULL, 10);
    return true;
}

static bool json_extract_bool(const char *json, const char *key, bool *out)
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
    while (*p && isspace((unsigned char)*p)) {
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

static esp_err_t save_config_file(void)
{
    FILE *f;

    if (!s_ctx.spiffs_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    f = fopen("/spiffs/config.json", "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open /spiffs/config.json for write");
        return ESP_FAIL;
    }

    fprintf(f,
            "{\n"
            "  \"defaultTime\": %d,\n"
            "  \"penalty\": %d,\n"
            "  \"maxTries\": %d,\n"
            "  \"wireCount\": %d,\n"
            "  \"debounceCheckIntervalMs\": %d,\n"
            "  \"debounceConsecutiveReads\": %d,\n"
            "  \"dedupeWindowMs\": %d,\n"
            "  \"holdResultSeconds\": %d,\n"
            "  \"heartbeatInterval\": %d,\n"
            "  \"keepSyncMaxDriftMs\": %d,\n"
            "  \"ledBrightnessPercent\": %d,\n"
            "  \"wifiApTimeoutSec\": %d,\n"
            "  \"deepSleepWindowSec\": %d,\n"
            "  \"maxIdleCycleSec\": %d,\n"
            "  \"timeToleranceMs\": %d,\n"
            "  \"lowBatteryCutoffPercent\": %d,\n"
            "  \"keepSyncEnabled\": %s,\n"
            "  \"mode\": \"%s\",\n"
            "  \"lidMode\": \"%s\",\n"
            "  \"solution\": \"%s\",\n"
            "  \"input1Name\": \"%s\",\n"
            "  \"input2Name\": \"%s\",\n"
            "  \"input3Name\": \"%s\",\n"
            "  \"input4Name\": \"%s\",\n"
            "  \"input5Name\": \"%s\",\n"
            "  \"input6Name\": \"%s\",\n"
            "  \"input7Name\": \"%s\",\n"
            "  \"input8Name\": \"%s\",\n"
            "  \"buzzerStartResumeMml\": \"%s\",\n"
            "  \"buzzerPauseResetMml\": \"%s\",\n"
            "  \"buzzerSolvedMml\": \"%s\",\n"
            "  \"buzzerFailedMml\": \"%s\"\n"
            "}\n",
            s_ctx.cfg.default_time_s,
            s_ctx.cfg.penalty_s,
            s_ctx.cfg.max_tries,
            s_ctx.cfg.wire_count,
            s_ctx.cfg.debounce_check_interval_ms,
            s_ctx.cfg.debounce_consecutive_reads,
            s_ctx.cfg.dedupe_window_ms,
            s_ctx.cfg.hold_result_s,
            heartbeat_ms_from_cfg(&s_ctx.cfg),
            s_ctx.cfg.keep_sync_max_drift_ms,
            s_ctx.cfg.led_brightness_percent,
            s_ctx.cfg.wifi_ap_timeout_s,
            s_ctx.cfg.deep_sleep_window_s,
            s_ctx.cfg.max_idle_cycle_s,
            s_ctx.cfg.time_tolerance_ms,
            s_ctx.low_battery_cutoff_percent,
            s_ctx.cfg.keep_sync_enabled ? "true" : "false",
            s_ctx.cfg.mode,
            s_ctx.cfg.lid_mode,
            s_ctx.cfg.solution,
            s_ctx.cfg.input_names[0],
            s_ctx.cfg.input_names[1],
            s_ctx.cfg.input_names[2],
            s_ctx.cfg.input_names[3],
            s_ctx.cfg.input_names[4],
            s_ctx.cfg.input_names[5],
            s_ctx.cfg.input_names[6],
            s_ctx.cfg.input_names[7],
            s_ctx.cfg.buzzer_start_resume_mml,
            s_ctx.cfg.buzzer_pause_reset_mml,
            s_ctx.cfg.buzzer_solved_mml,
            s_ctx.cfg.buzzer_failed_mml);

    fclose(f);
    return ESP_OK;
}

static esp_err_t save_battery_file(void)
{
    FILE *f;
    char points_csv[640];

    if (!s_ctx.spiffs_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    format_points_csv(points_csv, sizeof(points_csv));

    f = fopen(BATTERY_FILE_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for write", BATTERY_FILE_PATH);
        return ESP_FAIL;
    }

    fprintf(f,
            "{\n"
            "  \"profile\": \"%s\",\n"
            "  \"lowBatteryPercent\": %d,\n"
            "  \"shutdownDelaySec\": %d,\n"
            "  \"adcRaw\": %d,\n"
            "  \"adcAt0V\": %d,\n"
            "  \"adcAt15V\": %d,\n"
            "  \"simVoltageMv\": %d,\n"
            "  \"points\": \"%s\"\n"
            "}\n",
            s_ctx.battery_profile,
            s_ctx.low_battery_percent,
            s_ctx.battery_shutdown_delay_s,
            s_ctx.battery_adc_raw,
            s_ctx.battery_adc_at_0v,
            s_ctx.battery_adc_at_15v,
            s_ctx.battery_voltage_mv,
            points_csv);

    fclose(f);
    return ESP_OK;
}

static void apply_config_json_unlocked(const char *json)
{
    int i_val;
    bool b_val;
    char s_val[640];

    if (json_extract_int(json, "defaultTime", &i_val) && i_val >= 10 && i_val <= 24 * 3600) {
        s_ctx.cfg.default_time_s = i_val;
    }
    if (json_extract_int(json, "penalty", &i_val) && i_val >= 0 && i_val <= 3600) {
        s_ctx.cfg.penalty_s = i_val;
    }
    if (json_extract_int(json, "maxTries", &i_val) && i_val >= 1 && i_val <= 100) {
        s_ctx.cfg.max_tries = i_val;
    }
    if (json_extract_int(json, "wireCount", &i_val) && i_val >= 0 && i_val <= 8) {
        s_ctx.cfg.wire_count = i_val;
    }
    if (json_extract_int(json, "debounceCheckIntervalMs", &i_val) && i_val >= 1 && i_val <= 200) {
        s_ctx.cfg.debounce_check_interval_ms = i_val;
    }
    if (json_extract_int(json, "debounceConsecutiveReads", &i_val) && i_val >= 1 && i_val <= 20) {
        s_ctx.cfg.debounce_consecutive_reads = i_val;
    }
    if (json_extract_int(json, "dedupeWindowMs", &i_val) && i_val >= 100 && i_val <= 5000) {
        s_ctx.cfg.dedupe_window_ms = i_val;
    }
    if (json_extract_int(json, "holdResultSeconds", &i_val) && i_val >= 1 && i_val <= 1800) {
        s_ctx.cfg.hold_result_s = RESULT_RESET_DELAY_S;
    }
    s_ctx.cfg.hold_result_s = RESULT_RESET_DELAY_S;
    if (json_extract_int(json, "heartbeatInterval", &i_val)) {
        if (i_val >= 100 && i_val <= 120000) {
            s_ctx.cfg.heartbeat_interval_s = i_val / 1000;
            if (s_ctx.cfg.heartbeat_interval_s <= 0) {
                s_ctx.cfg.heartbeat_interval_s = 1;
            }
        } else if (i_val >= 1 && i_val <= 120) {
            s_ctx.cfg.heartbeat_interval_s = i_val;
        }
    }
    if (json_extract_int(json, "timeToleranceMs", &i_val) && i_val >= 0 && i_val <= 10000) {
        s_ctx.cfg.time_tolerance_ms = i_val;
    }
    if (json_extract_int(json, "lowBatteryCutoffPercent", &i_val)) {
        s_ctx.low_battery_cutoff_percent = clamp_int(i_val, 0, 100);
    }
    if (json_extract_int(json, "keepSyncMaxDriftMs", &i_val) && i_val >= 0 && i_val <= 10000) {
        s_ctx.cfg.keep_sync_max_drift_ms = i_val;
    }
    if (json_extract_int(json, "ledBrightnessPercent", &i_val) && i_val >= 1 && i_val <= 100) {
        s_ctx.cfg.led_brightness_percent = i_val;
    }
    if (json_extract_int(json, "wifiApTimeoutSec", &i_val) && i_val >= 5 && i_val <= 300) {
        s_ctx.cfg.wifi_ap_timeout_s = i_val;
    }
    if (json_extract_int(json, "deepSleepWindowSec", &i_val) && i_val >= 5 && i_val <= 3600) {
        s_ctx.cfg.deep_sleep_window_s = i_val;
    }
    if (json_extract_int(json, "maxIdleCycleSec", &i_val) && i_val >= 10 && i_val <= 7200) {
        s_ctx.cfg.max_idle_cycle_s = i_val;
    }

    if (json_extract_bool(json, "keepSyncEnabled", &b_val)) {
        s_ctx.cfg.keep_sync_enabled = b_val;
    }

    if (json_extract_string(json, "mode", s_val, sizeof(s_val))) {
        if (strcmp(s_val, "buzz") == 0 || strcmp(s_val, "penalty") == 0 || strcmp(s_val, "instant") == 0) {
            copy_bounded(s_ctx.cfg.mode, sizeof(s_ctx.cfg.mode), s_val);
        }
    }

    if (json_extract_string(json, "lidMode", s_val, sizeof(s_val))) {
        if (strcmp(s_val, "ignore") == 0) {
            copy_bounded(s_ctx.cfg.lid_mode, sizeof(s_ctx.cfg.lid_mode), "off");
        } else if (strcmp(s_val, "normallyClosed") == 0) {
            copy_bounded(s_ctx.cfg.lid_mode, sizeof(s_ctx.cfg.lid_mode), "closed");
        } else if (strcmp(s_val, "normallyOpen") == 0) {
            copy_bounded(s_ctx.cfg.lid_mode, sizeof(s_ctx.cfg.lid_mode), "open");
        } else if (strcmp(s_val, "off") == 0) {
            copy_bounded(s_ctx.cfg.lid_mode, sizeof(s_ctx.cfg.lid_mode), "off");
        } else if (strcmp(s_val, "closed") == 0) {
            copy_bounded(s_ctx.cfg.lid_mode, sizeof(s_ctx.cfg.lid_mode), "closed");
        } else if (strcmp(s_val, "open") == 0) {
            copy_bounded(s_ctx.cfg.lid_mode, sizeof(s_ctx.cfg.lid_mode), "open");
        }

        s_ctx.cfg.lid_enabled = strcmp(s_ctx.cfg.lid_mode, "off") != 0;
        s_ctx.cfg.lid_normally_closed = strcmp(s_ctx.cfg.lid_mode, "closed") == 0;
    }

    if (json_extract_string(json, "solution", s_val, sizeof(s_val))) {
        char clean[9] = {0};
        size_t len;
        bool valid;
        size_t i;
        (void)sanitize_solution_vector(s_val, clean, sizeof(clean));
        len = strlen(clean);
        valid = len > 0 && len <= 8;
        for (i = 0; i < len && valid; ++i) {
            int idx = clean[i] - '0';
            if (idx < 1 || idx > s_ctx.cfg.wire_count) {
                valid = false;
            }
        }
        if (valid) {
            copy_bounded(s_ctx.cfg.solution, sizeof(s_ctx.cfg.solution), clean);
        }
    }

    if (json_extract_string(json, "input1Name", s_val, sizeof(s_val))) {
        copy_bounded(s_ctx.cfg.input_names[0], sizeof(s_ctx.cfg.input_names[0]), s_val);
    }
    if (json_extract_string(json, "input2Name", s_val, sizeof(s_val))) {
        copy_bounded(s_ctx.cfg.input_names[1], sizeof(s_ctx.cfg.input_names[1]), s_val);
    }
    if (json_extract_string(json, "input3Name", s_val, sizeof(s_val))) {
        copy_bounded(s_ctx.cfg.input_names[2], sizeof(s_ctx.cfg.input_names[2]), s_val);
    }
    if (json_extract_string(json, "input4Name", s_val, sizeof(s_val))) {
        copy_bounded(s_ctx.cfg.input_names[3], sizeof(s_ctx.cfg.input_names[3]), s_val);
    }
    if (json_extract_string(json, "input5Name", s_val, sizeof(s_val))) {
        copy_bounded(s_ctx.cfg.input_names[4], sizeof(s_ctx.cfg.input_names[4]), s_val);
    }
    if (json_extract_string(json, "input6Name", s_val, sizeof(s_val))) {
        copy_bounded(s_ctx.cfg.input_names[5], sizeof(s_ctx.cfg.input_names[5]), s_val);
    }
    if (json_extract_string(json, "input7Name", s_val, sizeof(s_val))) {
        copy_bounded(s_ctx.cfg.input_names[6], sizeof(s_ctx.cfg.input_names[6]), s_val);
    }
    if (json_extract_string(json, "input8Name", s_val, sizeof(s_val))) {
        copy_bounded(s_ctx.cfg.input_names[7], sizeof(s_ctx.cfg.input_names[7]), s_val);
    }

    if (json_extract_string(json, "buzzerStartResumeMml", s_val, sizeof(s_val))) {
        copy_bounded(s_ctx.cfg.buzzer_start_resume_mml,
                     sizeof(s_ctx.cfg.buzzer_start_resume_mml),
                     s_val);
    }
    if (json_extract_string(json, "buzzerPauseResetMml", s_val, sizeof(s_val))) {
        copy_bounded(s_ctx.cfg.buzzer_pause_reset_mml,
                     sizeof(s_ctx.cfg.buzzer_pause_reset_mml),
                     s_val);
    }
    if (json_extract_string(json, "buzzerSolvedMml", s_val, sizeof(s_val))) {
        copy_bounded(s_ctx.cfg.buzzer_solved_mml,
                     sizeof(s_ctx.cfg.buzzer_solved_mml),
                     s_val);
    }
    if (json_extract_string(json, "buzzerFailedMml", s_val, sizeof(s_val))) {
        copy_bounded(s_ctx.cfg.buzzer_failed_mml,
                     sizeof(s_ctx.cfg.buzzer_failed_mml),
                     s_val);
    }

    if (json_extract_string(json, "batteryProfile", s_val, sizeof(s_val))) {
        const battery_profile_builtin_t *profile = find_builtin_profile(s_val);
        if (profile) {
            copy_bounded(s_ctx.battery_profile, sizeof(s_ctx.battery_profile), s_val);
            load_profile_points(profile);
        }
    }

    if (json_extract_int(json, "lowBatteryPercent", &i_val)) {
        s_ctx.low_battery_percent = clamp_int(i_val, 0, 100);
    }

    if (json_extract_int(json, "batteryVoltageMv", &i_val)) {
        s_ctx.battery_voltage_mv = clamp_int(i_val, 0, BATTERY_ADC_FULL_SCALE_MV);
        s_ctx.battery_adc_raw = battery_adc_raw_from_voltage_mv(s_ctx.battery_voltage_mv);
    }

    if (json_extract_int(json, "batteryAdcAt0V", &i_val)) {
        s_ctx.battery_adc_at_0v = clamp_int(i_val, 0, BATTERY_ADC_MAX_VALUE);
    }

    if (json_extract_int(json, "batteryAdcAt15V", &i_val)) {
        s_ctx.battery_adc_at_15v = clamp_int(i_val, 0, BATTERY_ADC_MAX_VALUE);
    }

    if (json_extract_int(json, "batteryAdcRaw", &i_val)) {
        s_ctx.battery_adc_raw = clamp_int(i_val, 0, BATTERY_ADC_MAX_VALUE);
    }

    if (json_extract_string(json, "batteryPoints", s_val, sizeof(s_val))) {
        battery_point_t pts[BATTERY_MAX_POINTS];
        int point_count = 0;
        if (parse_points_csv(s_val, pts, &point_count)) {
            int i;
            s_ctx.battery_point_count = point_count;
            for (i = 0; i < point_count; ++i) {
                s_ctx.battery_points[i] = pts[i];
            }
        }
    }

    {
        char clean[9] = {0};
        bool valid = sanitize_solution_vector(s_ctx.cfg.solution, clean, sizeof(clean));
        size_t i;

        for (i = 0; valid && clean[i] != '\0'; ++i) {
            int idx = clean[i] - '0';
            if (idx < 1 || idx > s_ctx.cfg.wire_count) {
                valid = false;
            }
        }

        if (valid && clean[0] != '\0') {
            copy_bounded(s_ctx.cfg.solution, sizeof(s_ctx.cfg.solution), clean);
        } else {
            int i_max = s_ctx.cfg.wire_count < 4 ? s_ctx.cfg.wire_count : 4;
            for (i = 0; i < (size_t)i_max; ++i) {
                clean[i] = (char)('1' + (int)i);
            }
            clean[i_max] = '\0';
            copy_bounded(s_ctx.cfg.solution, sizeof(s_ctx.cfg.solution), clean);
        }
    }

    update_battery_runtime_unlocked();
}

static void load_config_file_if_present(void)
{
    FILE *f;
    long size;
    char *buf;

    if (!s_ctx.spiffs_ready) {
        return;
    }

    f = fopen("/spiffs/config.json", "r");
    if (!f) {
        ESP_LOGI(TAG, "No persisted config file yet; using defaults");
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    size = ftell(f);
    if (size <= 0 || size > 8192) {
        fclose(f);
        return;
    }
    rewind(f);

    buf = (char *)calloc(1, (size_t)size + 1);
    if (!buf) {
        fclose(f);
        return;
    }

    if (fread(buf, 1, (size_t)size, f) == (size_t)size) {
        apply_config_json_unlocked(buf);
        ESP_LOGI(TAG, "Loaded persisted config from SPIFFS");
    }

    free(buf);
    fclose(f);
}

static void load_battery_file_if_present(void)
{
    FILE *f;
    long size;
    char *buf;

    if (!s_ctx.spiffs_ready) {
        return;
    }

    f = fopen(BATTERY_FILE_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No battery profile file yet; using defaults");
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    size = ftell(f);
    if (size <= 0 || size > 8192) {
        fclose(f);
        return;
    }
    rewind(f);

    buf = (char *)calloc(1, (size_t)size + 1);
    if (!buf) {
        fclose(f);
        return;
    }

    if (fread(buf, 1, (size_t)size, f) == (size_t)size) {
        int i_val;
        bool has_adc_raw = false;
        char s_val[640];

        if (json_extract_string(buf, "profile", s_val, sizeof(s_val))) {
            const battery_profile_builtin_t *profile = find_builtin_profile(s_val);
            if (profile) {
                copy_bounded(s_ctx.battery_profile, sizeof(s_ctx.battery_profile), s_val);
                load_profile_points(profile);
            }
        }

        if (json_extract_int(buf, "lowBatteryPercent", &i_val)) {
            s_ctx.low_battery_percent = clamp_int(i_val, 0, 100);
        }

        if (json_extract_int(buf, "shutdownDelaySec", &i_val)) {
            s_ctx.battery_shutdown_delay_s = clamp_int(i_val, 10, 600);
        }

        if (json_extract_int(buf, "adcAt0V", &i_val)) {
            s_ctx.battery_adc_at_0v = clamp_int(i_val, 0, BATTERY_ADC_MAX_VALUE);
        }

        if (json_extract_int(buf, "adcAt15V", &i_val)) {
            s_ctx.battery_adc_at_15v = clamp_int(i_val, 0, BATTERY_ADC_MAX_VALUE);
        }

        if (json_extract_int(buf, "adcRaw", &i_val)) {
            s_ctx.battery_adc_raw = clamp_int(i_val, 0, BATTERY_ADC_MAX_VALUE);
            has_adc_raw = true;
        }

        if (!has_adc_raw && json_extract_int(buf, "simVoltageMv", &i_val)) {
            s_ctx.battery_voltage_mv = clamp_int(i_val, 0, BATTERY_ADC_FULL_SCALE_MV);
            s_ctx.battery_adc_raw = battery_adc_raw_from_voltage_mv(s_ctx.battery_voltage_mv);
        }

        if (json_extract_string(buf, "points", s_val, sizeof(s_val))) {
            battery_point_t pts[BATTERY_MAX_POINTS];
            int point_count = 0;
            if (parse_points_csv(s_val, pts, &point_count)) {
                int i;
                s_ctx.battery_point_count = point_count;
                for (i = 0; i < point_count; ++i) {
                    s_ctx.battery_points[i] = pts[i];
                }
            }
        }

        update_battery_runtime_unlocked();
        ESP_LOGI(TAG, "Loaded battery profile from SPIFFS");
    }

    free(buf);
    fclose(f);
}

static void enter_result_state(prop_state_t target)
{
    prop_state_t prev = s_ctx.state;

    s_ctx.state = target;
    s_ctx.state_enter_ms = now_ms();
    if (prev != target) {
        ESP_LOGI(TAG, "State transition: %s -> %s", state_name(prev), state_name(target));
        if (target == PROP_STATE_DEFUSED) {
            queue_event_unlocked("defused");
        } else if (target == PROP_STATE_DETONATED) {
            queue_event_unlocked("detonated");
        }
    }
}

static void apply_wrong_wire_logic(void)
{
    int penalty_ms;

    s_ctx.penalty_until_ms = now_ms() + 3000;

    if (strcmp(s_ctx.cfg.mode, "instant") == 0) {
        enter_result_state(PROP_STATE_DETONATED);
        return;
    }

    s_ctx.tries_used++;
    if (s_ctx.tries_used >= s_ctx.cfg.max_tries) {
        enter_result_state(PROP_STATE_DETONATED);
        return;
    }

    if (strcmp(s_ctx.cfg.mode, "penalty") == 0) {
        penalty_ms = s_ctx.cfg.penalty_s * 1000;
        if (penalty_ms >= s_ctx.time_remaining_ms) {
            enter_result_state(PROP_STATE_DETONATED);
            return;
        }

        s_ctx.time_remaining_ms -= penalty_ms;
    }

    queue_event_unlocked("BAD-ATTEMPT");
}

static void handle_disconnect_unlocked(int idx)
{
    size_t order_len;
    size_t solution_len;
    char expected_ch;

    if (idx < 1 || idx > s_ctx.cfg.wire_count) {
        return;
    }

    if ((s_ctx.connected_mask & (1u << (idx - 1))) == 0) {
        return;
    }

    s_ctx.connected_mask &= (uint8_t)(~(1u << (idx - 1)));

    if (s_ctx.state == PROP_STATE_DETONATED) {
        return;
    }

    if (s_ctx.state != PROP_STATE_COUNTDOWN && s_ctx.state != PROP_STATE_PAUSED) {
        set_ready_state();
        return;
    }

    order_len = strlen(s_ctx.disconnected_order);
    solution_len = (size_t)solution_length_unlocked();
    if (order_len >= solution_len) {
        return;
    }

    expected_ch = s_ctx.cfg.solution[order_len];
    if ((char)('0' + idx) == expected_ch) {
        s_ctx.disconnected_order[order_len] = (char)('0' + idx);
        s_ctx.disconnected_order[order_len + 1] = '\0';

        if ((size_t)strlen(s_ctx.disconnected_order) >= solution_len) {
            enter_result_state(PROP_STATE_DEFUSED);
        }
        return;
    }

    apply_wrong_wire_logic();
}

static void handle_connect_unlocked(int idx)
{
    if (idx < 1 || idx > s_ctx.cfg.wire_count) {
        return;
    }

    if (strcmp(s_ctx.cfg.mode, "instant") == 0 && is_active_state()) {
        return;
    }

    s_ctx.connected_mask |= (uint8_t)(1u << (idx - 1));

    if (s_ctx.state == PROP_STATE_DETONATED) {
        return;
    }

    if (!is_active_state()) {
        set_ready_state();
    }
}

static bool command_is_deduped(const char *signature)
{
    int64_t ts = now_ms();
    if (!signature || !signature[0]) {
        return false;
    }

    if (strcmp(signature, s_ctx.last_cmd) == 0 && (ts - s_ctx.last_cmd_ms) < s_ctx.cfg.dedupe_window_ms) {
        return true;
    }
    copy_bounded(s_ctx.last_cmd, sizeof(s_ctx.last_cmd), signature);
    s_ctx.last_cmd_ms = ts;
    return false;
}

static void timer_task(void *arg)
{
    (void)arg;

    while (true) {
        bool should_deep_sleep = false;

        vTaskDelay(pdMS_TO_TICKS(100));

        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);

        if (s_ctx.state == PROP_STATE_COUNTDOWN) {
            s_ctx.time_remaining_ms -= 100;
            if (s_ctx.time_remaining_ms <= 0) {
                s_ctx.time_remaining_ms = 0;
                enter_result_state(PROP_STATE_DETONATED);
            }
        }

        if ((s_ctx.state == PROP_STATE_DEFUSED || s_ctx.state == PROP_STATE_DETONATED) &&
            (now_ms() - s_ctx.state_enter_ms) >= (int64_t)s_ctx.cfg.hold_result_s * 1000) {
            reset_round();
        }

        if ((now_ms() % 1000) < 120) {
            int64_t now = now_ms();
            update_battery_runtime_unlocked();

            if (s_ctx.battery_percent <= 0) {
                if (s_ctx.battery_zero_since_ms == 0) {
                    s_ctx.battery_zero_since_ms = now;
                }
                if ((now - s_ctx.battery_zero_since_ms) >=
                    (int64_t)s_ctx.battery_shutdown_delay_s * 1000) {
                    should_deep_sleep = true;
                }
            } else {
                s_ctx.battery_zero_since_ms = 0;
            }

            if (s_ctx.low_battery_cutoff_percent > 0 &&
                s_ctx.battery_percent <= s_ctx.low_battery_cutoff_percent) {
                if (s_ctx.battery_cutoff_since_ms == 0) {
                    s_ctx.battery_cutoff_since_ms = now;
                }
                if ((now - s_ctx.battery_cutoff_since_ms) >= LOW_BATTERY_CUTOFF_DELAY_MS) {
                    should_deep_sleep = true;
                }
            } else {
                s_ctx.battery_cutoff_since_ms = 0;
            }
        }

        xSemaphoreGive(s_ctx.lock);

        if (should_deep_sleep) {
            if (s_ctx.low_battery_cutoff_percent > 0 &&
                s_ctx.battery_percent <= s_ctx.low_battery_cutoff_percent) {
                ESP_LOGW(TAG,
                         "Battery stayed at or below cutoff (%d%% <= %d%%) for %d ms, entering deep sleep",
                         s_ctx.battery_percent,
                         s_ctx.low_battery_cutoff_percent,
                         LOW_BATTERY_CUTOFF_DELAY_MS);
            } else {
                ESP_LOGW(TAG, "Battery reached 0%% for %d seconds, entering deep sleep",
                         s_ctx.battery_shutdown_delay_s);
            }
            configure_deep_sleep_wake_gpio();
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_deep_sleep_start();
        }
    }
}

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 6,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        return err;
    }

    s_ctx.spiffs_ready = true;
    return ESP_OK;
}

esp_err_t prop_engine_init(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.lock = xSemaphoreCreateMutex();
    if (!s_ctx.lock) {
        return ESP_ERR_NO_MEM;
    }

    set_default_config(&s_ctx.cfg);
    set_default_battery_config();

    (void)init_spiffs();
    ESP_ERROR_CHECK(init_wire_ground_drive());
    ESP_ERROR_CHECK(init_wire_inputs());

    /* Small delay to let pins settle after config */
    vTaskDelay(pdMS_TO_TICKS(50));

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    load_config_file_if_present();
    load_battery_file_if_present();

    s_ctx.connected_mask = 0;
    for (int i = 0; i < 8; ++i) {
        int level = gpio_get_level(s_wire_input_gpios[i]);
        ESP_LOGI(TAG, "GPIO %d (input %d) level=%d", s_wire_input_gpios[i], i + 1, level);
        if (level == 0) {
            s_ctx.connected_mask |= (uint8_t)(1u << i);
        }
    }
    /* Also dump raw GPIO IN registers for diagnosis */
    {
        volatile uint32_t *gpio_in = (volatile uint32_t *)0x6000403C;  /* GPIO_IN_REG */
        volatile uint32_t *gpio_in1 = (volatile uint32_t *)0x60004040; /* GPIO_IN1_REG */
        ESP_LOGI(TAG, "GPIO_IN_REG=0x%08lX  GPIO_IN1_REG=0x%08lX", (unsigned long)*gpio_in, (unsigned long)*gpio_in1);
    }
    ESP_LOGI(TAG, "Initial connected_mask=0x%02X (wire_count=%d)", (unsigned)s_ctx.connected_mask, s_ctx.cfg.wire_count);
    s_ctx.time_remaining_ms = s_ctx.cfg.default_time_s * 1000;
    set_ready_state();
    xSemaphoreGive(s_ctx.lock);

    xTaskCreate(wire_input_task, "wire_inputs", 4096, NULL, 6, NULL);
    xTaskCreate(timer_task, "prop_timer", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Prop engine initialized");
    return ESP_OK;
}

void prop_engine_get_state_json(char *out, size_t out_size)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    prop_engine_get_state_json_unlocked(out, out_size);
    xSemaphoreGive(s_ctx.lock);
}

static void prop_engine_get_state_json_unlocked(char *out, size_t out_size)
{
    int64_t ts;
    ts = now_ms();

    snprintf(out,
             out_size,
             "{"
             "\"ts\":%lld,"
             "\"id\":\"px-wifi-v1\","
             "\"status\":\"online\","
             "\"gameState\":\"%s\","
             "\"timeRemaining\":%d,"
             "\"triesUsed\":%d,"
             "\"maxTries\":%d,"
             "\"mode\":\"%s\","
             "\"solution\":\"%s\","
             "\"disconnectedOrder\":\"%s\","
             "\"wireCount\":%d,"
             "\"connectedMask\":%u,"
             "\"battery\":%d,"
             "\"batteryVoltageMv\":%d,"
             "\"lowBattery\":%s"
             "}",
             (long long)ts,
             state_name(s_ctx.state),
             s_ctx.time_remaining_ms / 1000,
             s_ctx.tries_used,
             s_ctx.cfg.max_tries,
             s_ctx.cfg.mode,
             s_ctx.cfg.solution,
             s_ctx.disconnected_order,
             s_ctx.cfg.wire_count,
             (unsigned)s_ctx.connected_mask,
             s_ctx.battery_percent,
             s_ctx.battery_voltage_mv,
             s_ctx.battery_low ? "true" : "false");
}

void prop_engine_get_config_json(char *out, size_t out_size)
{
    const prop_config_t *cfg;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    cfg = &s_ctx.cfg;

    snprintf(out,
             out_size,
             "{"
             "\"defaultTime\":%d,"
             "\"penalty\":%d,"
             "\"maxTries\":%d,"
             "\"wireCount\":%d,"
             "\"mode\":\"%s\","
             "\"lidMode\":\"%s\","
             "\"solution\":\"%s\","
             "\"keepSyncEnabled\":%s,"
             "\"timeToleranceMs\":%d,"
             "\"batteryProfile\":\"%s\","
             "\"batteryAdcRaw\":%d,"
             "\"batteryAdcAt0V\":%d,"
             "\"batteryAdcAt15V\":%d,"
             "\"lowBatteryPercent\":%d,"
             "\"lowBatteryCutoffPercent\":%d,"
             "\"batteryVoltageMv\":%d,"
             "\"heartbeatInterval\":%d,"
             "\"input1Name\":\"%s\","
             "\"input2Name\":\"%s\","
             "\"input3Name\":\"%s\","
             "\"input4Name\":\"%s\","
             "\"input5Name\":\"%s\","
             "\"input6Name\":\"%s\","
             "\"input7Name\":\"%s\","
             "\"input8Name\":\"%s\","
             "\"buzzerStartResumeMml\":\"%s\","
             "\"buzzerPauseResetMml\":\"%s\","
             "\"buzzerSolvedMml\":\"%s\","
             "\"buzzerFailedMml\":\"%s\""
             "}",
             cfg->default_time_s,
             cfg->penalty_s,
             cfg->max_tries,
             cfg->wire_count,
             cfg->mode,
             cfg->lid_mode,
             cfg->solution,
             cfg->keep_sync_enabled ? "true" : "false",
             cfg->time_tolerance_ms,
             s_ctx.battery_profile,
             s_ctx.battery_adc_raw,
             s_ctx.battery_adc_at_0v,
             s_ctx.battery_adc_at_15v,
             s_ctx.low_battery_percent,
             s_ctx.low_battery_cutoff_percent,
             s_ctx.battery_voltage_mv,
             heartbeat_ms_from_cfg(cfg),
             cfg->input_names[0],
             cfg->input_names[1],
             cfg->input_names[2],
             cfg->input_names[3],
             cfg->input_names[4],
             cfg->input_names[5],
             cfg->input_names[6],
             cfg->input_names[7],
             cfg->buzzer_start_resume_mml,
             cfg->buzzer_pause_reset_mml,
             cfg->buzzer_solved_mml,
             cfg->buzzer_failed_mml);

    xSemaphoreGive(s_ctx.lock);
}

void prop_engine_get_default_config_json(char *out, size_t out_size)
{
    prop_config_t defaults;
    const battery_profile_builtin_t *profile = find_builtin_profile("unknown");

    set_default_config(&defaults);
    snprintf(out,
             out_size,
             "{"
             "\"defaultTime\":%d,"
             "\"penalty\":%d,"
             "\"maxTries\":%d,"
             "\"wireCount\":%d,"
             "\"mode\":\"%s\","
             "\"lidMode\":\"%s\","
             "\"solution\":\"%s\","
             "\"keepSyncEnabled\":%s,"
             "\"timeToleranceMs\":%d,"
             "\"batteryProfile\":\"%s\","
             "\"batteryAdcRaw\":%d,"
             "\"batteryAdcAt0V\":%d,"
             "\"batteryAdcAt15V\":%d,"
             "\"lowBatteryPercent\":%d,"
             "\"lowBatteryCutoffPercent\":%d,"
             "\"batteryVoltageMv\":%d,"
             "\"heartbeatInterval\":%d,"
             "\"input1Name\":\"%s\","
             "\"input2Name\":\"%s\","
             "\"input3Name\":\"%s\","
             "\"input4Name\":\"%s\","
             "\"input5Name\":\"%s\","
             "\"input6Name\":\"%s\","
             "\"input7Name\":\"%s\","
             "\"input8Name\":\"%s\","
             "\"buzzerStartResumeMml\":\"%s\","
             "\"buzzerPauseResetMml\":\"%s\","
             "\"buzzerSolvedMml\":\"%s\","
             "\"buzzerFailedMml\":\"%s\""
             "}",
             defaults.default_time_s,
             defaults.penalty_s,
             defaults.max_tries,
             defaults.wire_count,
             defaults.mode,
             defaults.lid_mode,
             defaults.solution,
             defaults.keep_sync_enabled ? "true" : "false",
             defaults.time_tolerance_ms,
             profile ? profile->name : "unknown",
             1420,
             0,
             BATTERY_ADC_MAX_VALUE,
             40,
             20,
             profile ? profile->points[0].mv - 80 : 6500,
             heartbeat_ms_from_cfg(&defaults),
             defaults.input_names[0],
             defaults.input_names[1],
             defaults.input_names[2],
             defaults.input_names[3],
             defaults.input_names[4],
             defaults.input_names[5],
             defaults.input_names[6],
             defaults.input_names[7],
             defaults.buzzer_start_resume_mml,
             defaults.buzzer_pause_reset_mml,
             defaults.buzzer_solved_mml,
             defaults.buzzer_failed_mml);
}

void prop_engine_get_buzzer_mml_config(prop_buzzer_mml_config_t *out)
{
    if (!out) {
        return;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    copy_bounded(out->start_resume, sizeof(out->start_resume), s_ctx.cfg.buzzer_start_resume_mml);
    copy_bounded(out->pause_reset, sizeof(out->pause_reset), s_ctx.cfg.buzzer_pause_reset_mml);
    copy_bounded(out->solved, sizeof(out->solved), s_ctx.cfg.buzzer_solved_mml);
    copy_bounded(out->failed, sizeof(out->failed), s_ctx.cfg.buzzer_failed_mml);
    xSemaphoreGive(s_ctx.lock);
}

void prop_engine_get_runtime_snapshot(prop_runtime_snapshot_t *out)
{
    if (!out) {
        return;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    out->state = s_ctx.state;
    out->time_remaining_ms = s_ctx.time_remaining_ms;
    out->connected_mask = s_ctx.connected_mask;
    out->wire_count = s_ctx.cfg.wire_count;
    out->ready_show_time = s_ctx.ready_show_time;
    out->stopped = s_ctx.stopped;
    copy_bounded(out->lid_mode, sizeof(out->lid_mode), s_ctx.cfg.lid_mode);
    xSemaphoreGive(s_ctx.lock);
}

bool prop_engine_pop_event_json(char *out, size_t out_size)
{
    prop_event_t event;

    if (!out || out_size < 8) {
        return false;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.event_head == s_ctx.event_tail) {
        xSemaphoreGive(s_ctx.lock);
        out[0] = '\0';
        return false;
    }

    event = s_ctx.event_queue[s_ctx.event_head];
    s_ctx.event_head = (s_ctx.event_head + 1) % PROP_EVENT_QUEUE_LEN;
    xSemaphoreGive(s_ctx.lock);

    snprintf(out,
             out_size,
             "{"
             "\"event\":\"%s\","
             "\"ts\":%lld,"
             "\"state\":\"%s\","
             "\"timeRemaining\":%d,"
             "\"triesUsed\":%d,"
             "\"maxTries\":%d,"
             "\"mode\":\"%s\""
             "}",
             event.name,
             (long long)event.ts_ms,
             state_name(event.state),
             event.time_remaining_ms / 1000,
             event.tries_used,
             event.max_tries,
             event.mode);
    return true;
}

static esp_err_t handle_command_unlocked(const char *cmd, const char *json, char *response, size_t response_size)
{
    int i_val;
    prop_state_t prev_state;

    ESP_LOGI(TAG, "Command received: %s (state=%s, t=%d)",
             cmd,
             state_name(s_ctx.state),
             s_ctx.time_remaining_ms / 1000);

    if (command_is_deduped((json && json[0]) ? json : cmd) && strcmp(cmd, "getState") != 0) {
        snprintf(response, response_size, "{\"ok\":true,\"deduped\":true}");
        return ESP_OK;
    }

    if (strcmp(cmd, "ping") == 0) {
        snprintf(response, response_size, "{\"event\":\"pong\",\"ts\":%lld}", (long long)now_ms());
        return ESP_OK;
    }

    if (strcmp(cmd, "getState") == 0) {
        prop_engine_get_state_json_unlocked(response, response_size);
        return ESP_OK;
    }

    if (strcmp(cmd, "start") == 0) {
        prev_state = s_ctx.state;

        if (json_extract_time_seconds(json, "time", &i_val) && i_val >= 0) {
            s_ctx.time_remaining_ms = i_val * 1000;
        }

        s_ctx.ready_show_time = false;
        s_ctx.stopped = false;

        if (s_ctx.state != PROP_STATE_READY) {
            set_ready_state();
            if (!all_wires_connected()) {
                build_not_ready_response("startIgnored",
                                         "Failed due to wires not connected",
                                         false,
                                         response,
                                         response_size);
            } else {
                snprintf(response,
                         response_size,
                         "{\"ok\":false,\"error\":\"notReady\",\"event\":\"startIgnored\","
                         "\"message\":\"Failed because prop is not in ready state\"}");
            }
            return ESP_OK;
        }

        if (s_ctx.time_remaining_ms <= 0) {
            s_ctx.time_remaining_ms = s_ctx.cfg.default_time_s * 1000;
        }

        s_ctx.state = PROP_STATE_COUNTDOWN;
        if (prev_state != s_ctx.state) {
            ESP_LOGI(TAG, "State transition: %s -> %s", state_name(prev_state), state_name(s_ctx.state));
        }
        snprintf(response, response_size, "{\"ok\":true,\"state\":\"countdown\"}");
        return ESP_OK;
    }

    if (strcmp(cmd, "resume") == 0) {
        prev_state = s_ctx.state;

        if (json_extract_time_seconds(json, "time", &i_val) && i_val >= 0) {
            s_ctx.time_remaining_ms = i_val * 1000;
        }

        s_ctx.ready_show_time = false;
        s_ctx.stopped = false;

        if (s_ctx.state == PROP_STATE_READY || s_ctx.state == PROP_STATE_NOT_READY) {
            set_ready_state();
            if (!all_wires_connected()) {
                build_not_ready_response("resumeIgnored",
                                         "Failed due to wires not connected",
                                         false,
                                         response,
                                         response_size);
                return ESP_OK;
            }

            if (s_ctx.time_remaining_ms <= 0) {
                s_ctx.time_remaining_ms = s_ctx.cfg.default_time_s * 1000;
            }

            s_ctx.state = PROP_STATE_COUNTDOWN;
            if (prev_state != s_ctx.state) {
                ESP_LOGI(TAG, "State transition: %s -> %s", state_name(prev_state), state_name(s_ctx.state));
            }
            snprintf(response, response_size, "{\"ok\":true,\"state\":\"countdown\"}");
            return ESP_OK;
        }

        if (s_ctx.state != PROP_STATE_PAUSED) {
            snprintf(response, response_size, "{\"ok\":false,\"error\":\"notPaused\"}");
            return ESP_OK;
        }

        if (!all_wires_connected()) {
            set_ready_state();
            build_not_ready_response("resumeIgnored",
                                     "Failed due to wires not connected",
                                     false,
                                     response,
                                     response_size);
            return ESP_OK;
        }

        if (s_ctx.time_remaining_ms <= 0) {
            s_ctx.time_remaining_ms = s_ctx.cfg.default_time_s * 1000;
        }

        s_ctx.state = PROP_STATE_COUNTDOWN;
        if (prev_state != s_ctx.state) {
            ESP_LOGI(TAG, "State transition: %s -> %s", state_name(prev_state), state_name(s_ctx.state));
        }
        snprintf(response, response_size, "{\"ok\":true,\"state\":\"countdown\"}");
        return ESP_OK;
    }

    if (strcmp(cmd, "pause") == 0 || strcmp(cmd, "stop") == 0) {
        if (s_ctx.state == PROP_STATE_COUNTDOWN) {
            prev_state = s_ctx.state;
            s_ctx.state = PROP_STATE_PAUSED;
            s_ctx.ready_show_time = false;
            s_ctx.stopped = (strcmp(cmd, "stop") == 0);
            ESP_LOGI(TAG, "State transition: %s -> %s", state_name(prev_state), state_name(s_ctx.state));
            snprintf(response, response_size, "{\"ok\":true,\"state\":\"paused\"}");
        } else {
            snprintf(response, response_size, "{\"ok\":false,\"error\":\"notRunning\"}");
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "reset") == 0) {
        ESP_LOGI(TAG, "Reset requested");
        s_ctx.penalty_until_ms = 0;
        if (json_extract_time_seconds(json, "time", &i_val) && i_val >= 0) {
            s_ctx.time_remaining_ms = i_val * 1000;
            s_ctx.tries_used = 0;
            s_ctx.disconnected_order[0] = '\0';
            s_ctx.stopped = false;
            set_ready_state();
            s_ctx.ready_show_time = (s_ctx.state == PROP_STATE_READY);
        } else {
            reset_round();
        }

        if (!all_wires_connected()) {
            build_not_ready_response("resetApplied",
                                     "Reset completed but prop is not ready",
                                     true,
                                     response,
                                     response_size);
        } else {
            snprintf(response,
                     response_size,
                     "{\"ok\":true,\"state\":\"%s\"}",
                     state_name(s_ctx.state));
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "setTime") == 0) {
        if (json_extract_time_seconds(json, "time", &i_val) && i_val >= 0) {
            s_ctx.time_remaining_ms = i_val * 1000;
            ESP_LOGI(TAG, "Time set: %d sec", i_val);
            snprintf(response, response_size, "{\"ok\":true,\"time\":%d}", i_val);
        } else {
            snprintf(response, response_size, "{\"ok\":false,\"error\":\"missingTime\"}");
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "setMode") == 0) {
        char mode[16];
        if (json_extract_string(json, "mode", mode, sizeof(mode)) &&
            (strcmp(mode, "buzz") == 0 || strcmp(mode, "penalty") == 0 || strcmp(mode, "instant") == 0)) {
            snprintf(s_ctx.cfg.mode, sizeof(s_ctx.cfg.mode), "%s", mode);
            if (json_extract_int(json, "maxTries", &i_val) && i_val >= 1 && i_val <= 100) {
                s_ctx.cfg.max_tries = i_val;
            }
            snprintf(response, response_size, "{\"ok\":true,\"mode\":\"%s\"}", s_ctx.cfg.mode);
        } else {
            snprintf(response, response_size, "{\"ok\":false,\"error\":\"invalidMode\"}");
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "setPenalty") == 0) {
        if (json_extract_int(json, "amount", &i_val) && i_val >= 0 && i_val <= 3600) {
            s_ctx.cfg.penalty_s = i_val;
            snprintf(response, response_size, "{\"ok\":true,\"penalty\":%d}", i_val);
        } else {
            snprintf(response, response_size, "{\"ok\":false,\"error\":\"invalidPenalty\"}");
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "setSequence") == 0) {
        char sol_raw[64];
        char sol[9];
        int wire_count = s_ctx.cfg.wire_count;

        if (json_extract_int(json, "wireCount", &i_val) && i_val >= 1 && i_val <= 8) {
            wire_count = i_val;
        }

        if (json_extract_string(json, "solution", sol_raw, sizeof(sol_raw))) {
            (void)sanitize_solution_vector(sol_raw, sol, sizeof(sol));
            size_t len = strlen(sol);
            bool valid = len > 0 && len <= 8;
            size_t i;
            for (i = 0; i < len && valid; ++i) {
                int idx = sol[i] - '0';
                if (idx < 1 || idx > wire_count) {
                    valid = false;
                }
            }
            if (!valid) {
                snprintf(response, response_size, "{\"ok\":false,\"error\":\"invalidSolution\"}");
                return ESP_OK;
            }
            s_ctx.cfg.wire_count = wire_count;
            snprintf(s_ctx.cfg.solution, sizeof(s_ctx.cfg.solution), "%s", sol);
            reset_round();
            s_ctx.connected_mask = (uint8_t)((1u << s_ctx.cfg.wire_count) - 1u);
            set_ready_state();
            snprintf(response, response_size, "{\"ok\":true,\"solution\":\"%s\"}", s_ctx.cfg.solution);
            return ESP_OK;
        }

        snprintf(response, response_size, "{\"ok\":false,\"error\":\"missingSolution\"}");
        return ESP_OK;
    }

    if (strcmp(cmd, "setLidMode") == 0) {
        char lid_mode[20];
        if (json_extract_string(json, "mode", lid_mode, sizeof(lid_mode))) {
            if (strcmp(lid_mode, "ignore") == 0) {
                copy_bounded(s_ctx.cfg.lid_mode, sizeof(s_ctx.cfg.lid_mode), "off");
            } else if (strcmp(lid_mode, "normallyClosed") == 0) {
                copy_bounded(s_ctx.cfg.lid_mode, sizeof(s_ctx.cfg.lid_mode), "closed");
            } else if (strcmp(lid_mode, "normallyOpen") == 0) {
                copy_bounded(s_ctx.cfg.lid_mode, sizeof(s_ctx.cfg.lid_mode), "open");
            } else if (strcmp(lid_mode, "off") == 0 || strcmp(lid_mode, "closed") == 0 || strcmp(lid_mode, "open") == 0) {
                copy_bounded(s_ctx.cfg.lid_mode, sizeof(s_ctx.cfg.lid_mode), lid_mode);
            } else {
                snprintf(response, response_size, "{\"ok\":false,\"error\":\"invalidLidMode\"}");
                return ESP_OK;
            }

            s_ctx.cfg.lid_enabled = strcmp(s_ctx.cfg.lid_mode, "off") != 0;
            s_ctx.cfg.lid_normally_closed = strcmp(s_ctx.cfg.lid_mode, "closed") == 0;
            snprintf(response, response_size, "{\"ok\":true,\"lidMode\":\"%s\"}", s_ctx.cfg.lid_mode);
        } else {
            snprintf(response, response_size, "{\"ok\":false,\"error\":\"missingMode\"}");
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "solve") == 0) {
        if (is_active_state()) {
            enter_result_state(PROP_STATE_DEFUSED);
            snprintf(response, response_size, "{\"ok\":true,\"state\":\"defused\"}");
        } else {
            snprintf(response, response_size, "{\"ok\":false,\"error\":\"notActive\"}");
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "fail") == 0) {
        if (is_active_state()) {
            s_ctx.time_remaining_ms = 0;
            enter_result_state(PROP_STATE_DETONATED);
            snprintf(response, response_size, "{\"ok\":true,\"state\":\"detonated\"}");
        } else {
            snprintf(response, response_size, "{\"ok\":false,\"error\":\"notActive\"}");
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "disconnect") == 0) {
        if (json_extract_int(json, "input", &i_val)) {
            handle_disconnect_unlocked(i_val);
            snprintf(response, response_size, "{\"ok\":true,\"input\":%d}", i_val);
        } else {
            snprintf(response, response_size, "{\"ok\":false,\"error\":\"missingInput\"}");
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "connect") == 0) {
        if (json_extract_int(json, "input", &i_val)) {
            handle_connect_unlocked(i_val);
            snprintf(response, response_size, "{\"ok\":true,\"input\":%d}", i_val);
        } else {
            snprintf(response, response_size, "{\"ok\":false,\"error\":\"missingInput\"}");
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "wake") == 0 || strcmp(cmd, "identify") == 0 || strcmp(cmd, "reboot") == 0) {
        snprintf(response, response_size, "{\"ok\":true,\"command\":\"%s\"}", cmd);
        return ESP_OK;
    }

    snprintf(response, response_size, "{\"ok\":false,\"error\":\"unknownCommand\"}");
    return ESP_OK;
}

esp_err_t prop_engine_handle_command_json(const char *json, char *response, size_t response_size)
{
    char command[24];

    if (!json || !response || response_size < 8) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!json_extract_string(json, "command", command, sizeof(command))) {
        snprintf(response, response_size, "{\"ok\":false,\"error\":\"missingCommand\"}");
        return ESP_OK;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    {
        esp_err_t err = handle_command_unlocked(command, json, response, response_size);
        xSemaphoreGive(s_ctx.lock);
        return err;
    }
}

esp_err_t prop_engine_apply_config_json(const char *json, bool persist, char *response, size_t response_size)
{
    esp_err_t err = ESP_OK;

    if (!json || !response || response_size < 8) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);

    apply_config_json_unlocked(json);
    if (!is_active_state()) {
        reset_round();
        s_ctx.connected_mask = (uint8_t)((1u << s_ctx.cfg.wire_count) - 1u);
        set_ready_state();
    }

    if (persist) {
        err = save_config_file();
        if (err == ESP_OK) {
            err = save_battery_file();
        }
    }

    snprintf(response,
             response_size,
             "{\"ok\":%s,\"persisted\":%s,\"spiffs\":%s}",
             err == ESP_OK ? "true" : "false",
             persist ? "true" : "false",
             s_ctx.spiffs_ready ? "true" : "false");

    xSemaphoreGive(s_ctx.lock);
    return ESP_OK;
}

esp_err_t prop_engine_restore_defaults(bool persist, char *response, size_t response_size)
{
    esp_err_t err = ESP_OK;

    if (!response || response_size < 8) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);

    set_default_config(&s_ctx.cfg);
    set_default_battery_config();
    if (!is_active_state()) {
        reset_round();
        s_ctx.connected_mask = (uint8_t)((1u << s_ctx.cfg.wire_count) - 1u);
        set_ready_state();
    }

    if (persist) {
        err = save_config_file();
        if (err == ESP_OK) {
            err = save_battery_file();
        }
    }

    snprintf(response,
             response_size,
             "{\"ok\":%s,\"restored\":true,\"persisted\":%s,\"spiffs\":%s}",
             err == ESP_OK ? "true" : "false",
             persist ? "true" : "false",
             s_ctx.spiffs_ready ? "true" : "false");

    xSemaphoreGive(s_ctx.lock);
    return ESP_OK;
}

prop_led_hint_t prop_engine_get_led_hint(void)
{
    prop_led_hint_t hint;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    hint = led_hint_unlocked();
    xSemaphoreGive(s_ctx.lock);

    return hint;
}

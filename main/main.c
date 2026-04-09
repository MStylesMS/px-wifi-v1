#include "board.h"
#include "px_system.h"
#include "drv_rgb_led.h"
#include "web_ui.h"
#include "prop_engine.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "px-wifi-v1";

static uint8_t lerp_u8(uint8_t a, uint8_t b, int num, int den)
{
    return (uint8_t)(a + ((b - a) * num) / den);
}

static rgb_color_t scale_color(rgb_color_t c, uint8_t level)
{
    rgb_color_t out;
    out.r = (uint8_t)((c.r * level) / 255);
    out.g = (uint8_t)((c.g * level) / 255);
    out.b = (uint8_t)((c.b * level) / 255);
    return out;
}

static uint8_t pulse_level_1hz(int64_t t_ms)
{
    int phase = (int)(t_ms % 1000);
    int tri = phase < 500 ? phase : (1000 - phase);
    return lerp_u8(32, 255, tri, 500);
}

static bool blink_4hz_on(int64_t t_ms)
{
    return ((t_ms % 250) < 125);
}

static bool double_blink_on(int64_t t_ms)
{
    int phase = (int)(t_ms % 1200);
    return (phase < 100) || (phase >= 220 && phase < 320);
}

static rgb_color_t led_color_for_hint(prop_led_hint_t hint, int64_t t_ms)
{
    const rgb_color_t red = {31, 0, 0};
    const rgb_color_t yellow = {31, 31, 0};
    const rgb_color_t green = {0, 31, 0};
    const rgb_color_t cyan = {0, 31, 31};
    const rgb_color_t blue = {0, 0, 31};
    const rgb_color_t magenta = {31, 0, 31};
    const rgb_color_t white = {31, 31, 31};
    const rgb_color_t orange = {31, 12, 0};
    const rgb_color_t off = {0, 0, 0};

    switch (hint) {
        case PROP_LED_HINT_AP_MODE:
            return magenta;
        case PROP_LED_HINT_CONNECTING_WIFI:
            return scale_color(blue, pulse_level_1hz(t_ms));
        case PROP_LED_HINT_CONNECTING_MQTT:
            return double_blink_on(t_ms) ? cyan : off;
        case PROP_LED_HINT_READY:
            return green;
        case PROP_LED_HINT_NOT_READY:
            return scale_color(yellow, pulse_level_1hz(t_ms));
        case PROP_LED_HINT_COUNTDOWN:
            return scale_color(white, pulse_level_1hz(t_ms));
        case PROP_LED_HINT_PAUSED:
            return blink_4hz_on(t_ms) ? yellow : off;
        case PROP_LED_HINT_PENALTY:
            return red;
        case PROP_LED_HINT_DETONATED:
            return blink_4hz_on(t_ms) ? red : off;
        case PROP_LED_HINT_DEFUSED:
            return blink_4hz_on(t_ms) ? green : off;
        case PROP_LED_HINT_OTA:
            return double_blink_on(t_ms) ? orange : off;
        case PROP_LED_HINT_OFF:
        default:
            return off;
    }
}

static void led_task(void *arg)
{
    (void)arg;

    while (true) {
        prop_led_hint_t hint = prop_engine_get_led_hint();
        int64_t t_ms = esp_timer_get_time() / 1000;
        rgb_color_t c = led_color_for_hint(hint, t_ms);
        (void)drv_rgb_led_set(0, c);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting px-wifi-v1");

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Build info: id=%s date=%s time=%s", app->version, app->date, app->time);

    ESP_ERROR_CHECK(px_system_init());

    drv_rgb_led_config_t led_cfg = {
        .gpio_num = BOARD_RGB_LED_GPIO,
        .max_leds = BOARD_RGB_LED_COUNT,
    };
    ESP_ERROR_CHECK(drv_rgb_led_init(&led_cfg));

    ESP_ERROR_CHECK(prop_engine_init());
    xTaskCreate(led_task, "led_status", 3072, NULL, 5, NULL);
    ESP_ERROR_CHECK(web_ui_start());
}

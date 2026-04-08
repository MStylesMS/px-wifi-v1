#include "board.h"
#include "px_system.h"
#include "drv_rgb_led.h"
#include "esp_log.h"

static const char *TAG = "px-wifi-v1";

static const rgb_color_t colors[] = {
    {  0,   0,   0},   /* off     */
    {255,   0,   0},   /* red     */
    {255, 255,   0},   /* yellow  */
    {  0, 255,   0},   /* green   */
    {  0, 255, 255},   /* cyan    */
    {  0,   0, 255},   /* blue    */
    {255,   0, 255},   /* magenta */
    {  0,   0,   0},   /* off     */
    {255, 255, 255},   /* white   */
};

#define NUM_COLORS  (sizeof(colors) / sizeof(colors[0]))
#define PERIOD_MS   9000
#define STEPS       100

void app_main(void)
{
    ESP_LOGI(TAG, "Starting px-wifi-v1");

    ESP_ERROR_CHECK(px_system_init());

    drv_rgb_led_config_t led_cfg = {
        .gpio_num = BOARD_RGB_LED_GPIO,
        .max_leds = BOARD_RGB_LED_COUNT,
    };
    ESP_ERROR_CHECK(drv_rgb_led_init(&led_cfg));

    drv_rgb_led_cycle(colors, NUM_COLORS, PERIOD_MS, STEPS);
}

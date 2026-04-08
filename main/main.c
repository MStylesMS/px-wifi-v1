#include "board.h"
#include "px_system.h"
#include "drv_rgb_led.h"
#include "web_ui.h"
#include "esp_log.h"

static const char *TAG = "px-wifi-v1";

static const rgb_color_t colors[] = {
    {  0,   0,   0},   /* off     */
    {127,   0,   0},   /* red     */
    {127, 127,   0},   /* yellow  */
    {  0, 127,   0},   /* green   */
    {  0, 127, 127},   /* cyan    */
    {  0,   0, 127},   /* blue    */
    {127,   0, 127},   /* magenta */
    {  0,   0,   0},   /* off     */
    {127, 127, 127},   /* white   */
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

    ESP_ERROR_CHECK(web_ui_start());

    drv_rgb_led_cycle(colors, NUM_COLORS, PERIOD_MS, STEPS);
}

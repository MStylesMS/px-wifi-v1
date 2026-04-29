#include "board.h"
#include "px_system.h"
#include "drv_rgb_led.h"
#include "web_ui.h"
#include "prop_engine.h"
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "px-wifi-v1";

#define PIEZO_GPIO      14
#define BUZZER_DUTY_50  512
#define BUZZER_MAX_NOTES 96

#define DISP_I2C_PORT I2C_NUM_0
#define DISP_I2C_SDA 1
#define DISP_I2C_SCL 2
#define DISP_I2C_FREQ_HZ 100000
#define DISP_HT16K33_ADDR_DEFAULT 0x70
#define DEEP_SLEEP_WAKE_GPIO 4

#if BOARD_RGB_LED_COUNT < 1 || BOARD_RGB_LED_COUNT > DRV_RGB_LED_MAX_LEDS
#error "BOARD_RGB_LED_COUNT must be in range 1..DRV_RGB_LED_MAX_LEDS"
#endif

#define SEG_A 0x01
#define SEG_B 0x02
#define SEG_C 0x04
#define SEG_D 0x08
#define SEG_E 0x10
#define SEG_F 0x20
#define SEG_G 0x40
#define SEG_DP 0x80

static bool s_display_ready;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_display_dev;
static uint8_t s_display_addr = DISP_HT16K33_ADDR_DEFAULT;

static uint8_t lerp_u8(uint8_t a, uint8_t b, int num, int den)
{
    return (uint8_t)(a + ((b - a) * num) / den);
}

static const char *wakeup_cause_name(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            return "undefined";
        case ESP_SLEEP_WAKEUP_EXT0:
            return "ext0";
        case ESP_SLEEP_WAKEUP_EXT1:
            return "ext1";
        case ESP_SLEEP_WAKEUP_TIMER:
            return "timer";
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            return "touchpad";
        case ESP_SLEEP_WAKEUP_ULP:
            return "ulp";
        case ESP_SLEEP_WAKEUP_GPIO:
            return "gpio";
        case ESP_SLEEP_WAKEUP_UART:
            return "uart";
        case ESP_SLEEP_WAKEUP_WIFI:
            return "wifi";
        case ESP_SLEEP_WAKEUP_COCPU:
            return "cocpu";
        case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:
            return "cocpu_trap";
        case ESP_SLEEP_WAKEUP_BT:
            return "bt";
        default:
            return "other";
    }
}

static void log_boot_wakeup_info(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    ESP_LOGI(TAG, "Wakeup cause: %s (%d)", wakeup_cause_name(cause), (int)cause);

    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        uint64_t ext1_mask = esp_sleep_get_ext1_wakeup_status();
        bool wake_gpio_triggered = (ext1_mask & (1ULL << DEEP_SLEEP_WAKE_GPIO)) != 0;

        ESP_LOGI(TAG,
                 "EXT1 wake mask: 0x%llx, gpio%d_triggered=%s",
                 (unsigned long long)ext1_mask,
                 DEEP_SLEEP_WAKE_GPIO,
                 wake_gpio_triggered ? "true" : "false");
    }
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

static bool display_i2c_write_cmd(uint8_t cmd)
{
    esp_err_t err = i2c_master_transmit(s_display_dev, &cmd, 1, 25);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HT16K33 cmd 0x%02X failed: %s", cmd, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static bool display_i2c_write_frame(const uint16_t frame[8])
{
    uint8_t buf[17];
    int i;

    buf[0] = 0x00;
    for (i = 0; i < 8; ++i) {
        buf[1 + i * 2] = (uint8_t)(frame[i] & 0xFF);
        buf[1 + i * 2 + 1] = (uint8_t)((frame[i] >> 8) & 0xFF);
    }

    esp_err_t err = i2c_master_transmit(s_display_dev, buf, sizeof(buf), 25);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HT16K33 frame write failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

/* Re-send the HT16K33 wakeup/display-on sequence without extra logging. */
static bool display_wake(void)
{
    return display_i2c_write_cmd(0x21) &&  /* oscillator on */
           display_i2c_write_cmd(0x81) &&  /* display on, no blink */
           display_i2c_write_cmd(0xEF);    /* max brightness */
}

/* Reset the I2C bus (releases any stuck slave), then re-arm display.
 * Retries up to max_retries times with increasing delays.
 * Returns true when the display responds successfully. */
static bool display_ensure_wake(int max_retries)
{
    for (int i = 0; i < max_retries; i++) {
        i2c_master_bus_reset(s_i2c_bus);
        vTaskDelay(pdMS_TO_TICKS(50 + i * 50));
        if (display_wake()) {
            if (i > 0) {
                ESP_LOGI(TAG, "HT16K33 recovered after %d reset(s)", i + 1);
            }
            return true;
        }
        ESP_LOGW(TAG, "HT16K33 unresponsive, reset attempt %d/%d", i + 1, max_retries);
    }
    return false;
}

static uint16_t seg_for_digit(int d)
{
    switch (d) {
        case 0: return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
        case 1: return SEG_B | SEG_C;
        case 2: return SEG_A | SEG_B | SEG_D | SEG_E | SEG_G;
        case 3: return SEG_A | SEG_B | SEG_C | SEG_D | SEG_G;
        case 4: return SEG_B | SEG_C | SEG_F | SEG_G;
        case 5: return SEG_A | SEG_C | SEG_D | SEG_F | SEG_G;
        case 6: return SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
        case 7: return SEG_A | SEG_B | SEG_C;
        case 8: return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
        case 9: return SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G;
        default: return 0;
    }
}

static void display_set_digit(uint16_t frame[8], int pos, uint16_t segments)
{
    static const int idx_map[4] = {0, 1, 3, 4};
    if (pos < 0 || pos > 3) {
        return;
    }
    frame[idx_map[pos]] = segments;
}

static void display_render_blank(uint16_t frame[8])
{
    memset(frame, 0, sizeof(uint16_t) * 8);
}

static void display_render_dashes(uint16_t frame[8])
{
    int i;
    display_render_blank(frame);
    for (i = 0; i < 4; ++i) {
        display_set_digit(frame, i, SEG_G);
    }
}

static void display_render_ready_chase(uint16_t frame[8], int64_t now_ms)
{
    int dot_idx;

    display_render_blank(frame);

    /* One dot advances once per second, left to right across 4 digits. */
    dot_idx = (int)((now_ms / 1000) % 4);
    display_set_digit(frame, dot_idx, SEG_DP);
}

static void display_render_mmss(uint16_t frame[8], int total_seconds, bool colon_on)
{
    int mm;
    int ss;

    if (total_seconds < 0) {
        total_seconds = 0;
    }
    if (total_seconds > 99 * 60 + 59) {
        total_seconds = 99 * 60 + 59;
    }

    mm = total_seconds / 60;
    ss = total_seconds % 60;

    display_render_blank(frame);
    display_set_digit(frame, 0, seg_for_digit((mm / 10) % 10));
    display_set_digit(frame, 1, seg_for_digit(mm % 10));
    display_set_digit(frame, 2, seg_for_digit((ss / 10) % 10));
    display_set_digit(frame, 3, seg_for_digit(ss % 10));
    if (colon_on) {
        frame[2] |= 0x02;
    }
}

static void display_apply_progress_bars(uint16_t frame[8], uint8_t connected_mask, int wire_count)
{
    int i;
    for (i = 0; i < 8; ++i) {
        bool used = i < wire_count;
        bool connected = (connected_mask & (1u << i)) != 0;

        if (!used || !connected) {
            continue;
        }

        if (i < 4) {
            static const int idx_map[4] = {0, 1, 3, 4};
            frame[idx_map[i]] |= SEG_A;
        } else {
            static const int idx_map[4] = {0, 1, 3, 4};
            frame[idx_map[i - 4]] |= SEG_D;
        }
    }
}

/* Get WiFi signal strength in dots (1-4) based on RSSI.
   1 dot = no connection or very poor (<-90 dBm)
   2 dots = weak (-80 to -90 dBm)
   3 dots = medium (-60 to -80 dBm)
   4 dots = strong (>-60 dBm) */
static int get_wifi_strength_dots(void)
{
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        return 1;  /* no connection */
    }
    
    int rssi = ap_info.rssi;
    if (rssi > -60) return 4;
    if (rssi > -80) return 3;
    if (rssi > -90) return 2;
    return 1;
}

/* Render WiFi strength indicator: N dots blinking at 1 Hz */
static void display_render_wifi_strength(uint16_t frame[8], int64_t now_ms)
{
    int dots = get_wifi_strength_dots();
    bool blink_on = ((now_ms / 1000) % 2) == 0;
    int i;
    
    display_render_blank(frame);
    
    if (blink_on) {
        for (i = 0; i < dots && i < 4; ++i) {
            display_set_digit(frame, i, SEG_DP);
        }
    }
}

static bool lid_forces_blank(const char *lid_mode)
{
    int level;

    if (!lid_mode || strcmp(lid_mode, "off") == 0) {
        return false;
    }

    level = gpio_get_level(GPIO_NUM_18);
    if (strcmp(lid_mode, "closed") == 0) {
        return level == 0;
    }
    if (strcmp(lid_mode, "open") == 0) {
        return level == 1;
    }

    return false;
}

static void display_task(void *arg)
{
    (void)arg;

    /* Wait for WiFi radio activity to settle before attempting I2C. */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Ensure the HT16K33 is armed, retrying through any RF-induced glitches. */
    if (!display_ensure_wake(10)) {
        ESP_LOGE(TAG, "HT16K33 unresponsive after 10 resets — display task exiting");
        vTaskDelete(NULL);
        return;
    }

    uint16_t last_frame[8] = {0xFFFF};
    prop_runtime_snapshot_t snap;
    prop_state_t prev_state = PROP_STATE_NOT_READY;
    int frozen_result_seconds = 0;
    int last_live_seconds = 0;
    int64_t result_enter_ms = 0;

    while (true) {
        uint16_t frame[8];
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool colon_on_1hz = ((now_ms / 1000) % 2) == 0;
        uint32_t refresh_ms = 100;  /* default faster refresh */

        prop_engine_get_runtime_snapshot(&snap);

        if (snap.state == PROP_STATE_COUNTDOWN || snap.state == PROP_STATE_PAUSED) {
            last_live_seconds = snap.time_remaining_ms / 1000;
            if (last_live_seconds < 0) {
                last_live_seconds = 0;
            }
        }

        if (snap.state != prev_state &&
            (snap.state == PROP_STATE_DEFUSED || snap.state == PROP_STATE_DETONATED)) {
            if (prev_state == PROP_STATE_COUNTDOWN || prev_state == PROP_STATE_PAUSED) {
                frozen_result_seconds = last_live_seconds;
            } else {
                frozen_result_seconds = snap.time_remaining_ms / 1000;
            }
            if (frozen_result_seconds < 0) {
                frozen_result_seconds = 0;
            }
            result_enter_ms = now_ms;
        }
        prev_state = snap.state;

        if (lid_forces_blank(snap.lid_mode)) {
            display_render_blank(frame);
        } else if (snap.state == PROP_STATE_READY) {
            /* In READY state, show WiFi strength indicator and use slower refresh
             * to allow light sleep to be more effective. */
            display_render_wifi_strength(frame, now_ms);
            refresh_ms = 1000;  /* Update once per second */
        } else if (snap.state == PROP_STATE_NOT_READY) {
            display_render_dashes(frame);
            display_apply_progress_bars(frame, snap.connected_mask, snap.wire_count);
        } else if (snap.state == PROP_STATE_COUNTDOWN || snap.state == PROP_STATE_PAUSED) {
            display_render_mmss(frame, snap.time_remaining_ms / 1000, colon_on_1hz);
        } else if (snap.state == PROP_STATE_DEFUSED || snap.state == PROP_STATE_DETONATED) {
            if ((now_ms - result_enter_ms) <= 120000) {
                display_render_mmss(frame, frozen_result_seconds, true);
            } else {
                display_render_blank(frame);
            }
        } else {
            display_render_blank(frame);
        }

        if (memcmp(frame, last_frame, sizeof(frame)) != 0) {
            if (!display_i2c_write_frame(frame)) {
                /* Write failed — reset the bus and re-arm, then force a retry next cycle. */
                display_ensure_wake(3);
                memset(last_frame, 0xFF, sizeof(last_frame));
            } else {
                memcpy(last_frame, frame, sizeof(frame));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(refresh_ms));
    }
}

static void init_display(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = DISP_I2C_PORT,
        .sda_io_num = DISP_I2C_SDA,
        .scl_io_num = DISP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DISP_HT16K33_ADDR_DEFAULT,
        .scl_speed_hz = DISP_I2C_FREQ_HZ,
    };
    int addr;
    bool found = false;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "I2C probe 0x70-0x77 (SDA=GPIO%d SCL=GPIO%d @ %dHz)",
             DISP_I2C_SDA, DISP_I2C_SCL, DISP_I2C_FREQ_HZ);
    int nack_count = 0, timeout_count = 0, other_count = 0;
    for (addr = 0x70; addr <= 0x77; ++addr) {
        err = i2c_master_probe(s_i2c_bus, addr, 50);
        if (err == ESP_OK) {
            s_display_addr = (uint8_t)addr;
            found = true;
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
            break;
        } else if (err == ESP_ERR_NOT_FOUND) {
            nack_count++;
        } else if (err == ESP_ERR_TIMEOUT) {
            timeout_count++;
        } else {
            other_count++;
        }
    }

    if (!found) {
        if (timeout_count > 0) {
            ESP_LOGW(TAG, "No I2C device found at 0x70-0x77 (bus timeout; check SDA/SCL/power)");
        } else if (nack_count > 0) {
            ESP_LOGW(TAG, "No I2C device found at 0x70-0x77 (bus alive, address mismatch?)");
        } else {
            ESP_LOGW(TAG, "No I2C device found at 0x70-0x77 (unexpected probe error)");
        }
        ESP_LOGI(TAG, "I2C probe summary: NACK=%d TIMEOUT=%d OTHER=%d", nack_count, timeout_count, other_count);
        return;
    }

    dev_cfg.device_address = s_display_addr;
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_display_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C add display device 0x%02X failed: %s", s_display_addr, esp_err_to_name(err));
        return;
    }

    if (!display_i2c_write_cmd(0x21)) {
        ESP_LOGW(TAG, "HT16K33 oscillator enable failed");
        return;
    }
    if (!display_i2c_write_cmd(0x81)) {
        ESP_LOGW(TAG, "HT16K33 display-on command failed");
        return;
    }
    if (!display_i2c_write_cmd(0xEF)) {
        ESP_LOGW(TAG, "HT16K33 brightness command failed");
        return;
    }

    s_display_ready = true;
    ESP_LOGI(TAG, "HT16K33 display initialized on I2C addr 0x%02X", s_display_addr);
}

/* ---------- buzzer MML sequencer ---------- */

typedef struct {
    uint32_t freq_hz;     /* 0 = silence */
    uint32_t duration_ms;
    uint16_t duty;        /* 0..1023 */
} buzzer_note_t;

typedef struct {
    buzzer_note_t notes[BUZZER_MAX_NOTES];
    int           len;
    int           idx;
    int64_t       note_start_ms;
    bool          active;
} buzzer_player_t;

static buzzer_player_t s_player;

static uint16_t duty_from_volume(int volume)
{
    int v = volume;
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    return (uint16_t)((1023 * v) / 100);
}

static int parse_uint(const char **pp)
{
    const char *p = *pp;
    int v = 0;
    bool any = false;

    while (*p && isdigit((unsigned char)*p)) {
        any = true;
        v = (v * 10) + (*p - '0');
        p++;
    }
    *pp = p;
    return any ? v : -1;
}

static int semitone_for_note(char note)
{
    switch (note) {
        case 'C': return 0;
        case 'D': return 2;
        case 'E': return 4;
        case 'F': return 5;
        case 'G': return 7;
        case 'A': return 9;
        case 'B': return 11;
        default:  return 0;
    }
}

static uint32_t duration_ms_from_len(int tempo, int note_len, bool dotted)
{
    if (tempo < 30) {
        tempo = 30;
    }
    if (tempo > 400) {
        tempo = 400;
    }
    if (note_len < 1) {
        note_len = 1;
    }

    /* quarter note = 60000 / tempo */
    uint32_t d = (uint32_t)((60000UL * 4UL) / ((uint32_t)tempo * (uint32_t)note_len));
    if (dotted) {
        d += d / 2;
    }
    if (d < 15) {
        d = 15;
    }
    return d;
}

static bool parse_mml_song(const char *mml, buzzer_note_t *out, int max_notes, int *out_len)
{
    int tempo = 120;
    int octave = 5;
    int default_len = 4;
    int volume = 70;
    const char *p = mml;
    int n = 0;

    if (!mml || !out || !out_len || max_notes <= 0) {
        return false;
    }

    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';')) {
            p++;
        }
        if (!*p) {
            break;
        }

        char c = (char)toupper((unsigned char)*p);
        if (c == 'T' || c == 'O' || c == 'L' || c == 'V') {
            int v;
            p++;
            v = parse_uint(&p);
            if (v >= 0) {
                if (c == 'T') {
                    tempo = v;
                } else if (c == 'O') {
                    if (v < 0) {
                        v = 0;
                    }
                    if (v > 8) {
                        v = 8;
                    }
                    octave = v;
                } else if (c == 'L') {
                    default_len = v;
                } else if (c == 'V') {
                    volume = v;
                }
            }
            continue;
        }

        if (c == '>') {
            p++;
            if (octave < 8) {
                octave++;
            }
            continue;
        }
        if (c == '<') {
            p++;
            if (octave > 0) {
                octave--;
            }
            continue;
        }

        if ((c >= 'A' && c <= 'G') || c == 'R') {
            int accidental = 0;
            int note_len;
            bool dotted = false;
            int note_octave = octave;
            buzzer_note_t ev;

            p++;

            if (c != 'R') {
                if (*p == '#' || *p == '+') {
                    accidental = 1;
                    p++;
                } else if (*p == '-') {
                    accidental = -1;
                    p++;
                }
            }

            note_len = parse_uint(&p);
            if (note_len < 1) {
                note_len = default_len;
            }

            if (isdigit((unsigned char)*p)) {
                int explicit_oct = *p - '0';
                if (explicit_oct >= 0 && explicit_oct <= 8) {
                    note_octave = explicit_oct;
                }
                p++;
            }

            if (*p == '.') {
                dotted = true;
                p++;
            }

            ev.duration_ms = duration_ms_from_len(tempo, note_len, dotted);
            if (c == 'R') {
                ev.freq_hz = 0;
                ev.duty = 0;
            } else {
                int semitone = semitone_for_note(c) + accidental;
                int midi = (note_octave + 1) * 12 + semitone;
                if (midi < 12) {
                    midi = 12;
                }
                if (midi > 119) {
                    midi = 119;
                }
                ev.freq_hz = (uint32_t)(440.0f * powf(2.0f, (float)(midi - 69) / 12.0f) + 0.5f);
                ev.duty = duty_from_volume(volume);
            }

            if (n < max_notes) {
                out[n++] = ev;
            }
            continue;
        }

        /* Ignore unknown token and keep parsing */
        p++;
    }

    *out_len = n;
    return n > 0;
}

static void buzzer_set(bool on, uint32_t freq_hz, uint16_t duty)
{
    if (!on || freq_hz == 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        return;
    }
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq_hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void buzzer_play_mml(const char *mml)
{
    if (!parse_mml_song(mml, s_player.notes, BUZZER_MAX_NOTES, &s_player.len)) {
        s_player.active = false;
        buzzer_set(false, 0, 0);
        ESP_LOGW(TAG, "Invalid/empty MML song; skipped");
        return;
    }

    s_player.idx           = 0;
    s_player.note_start_ms = esp_timer_get_time() / 1000;
    s_player.active        = true;
    buzzer_set(s_player.notes[0].freq_hz != 0, s_player.notes[0].freq_hz, s_player.notes[0].duty);
}

/* Call every ~20 ms; returns true while song is still playing. */
static bool buzzer_tick(void)
{
    if (!s_player.active) {
        return false;
    }
    int64_t now = esp_timer_get_time() / 1000;
    const buzzer_note_t *note = &s_player.notes[s_player.idx];

    if ((now - s_player.note_start_ms) >= note->duration_ms) {
        s_player.idx++;
        if (s_player.idx >= s_player.len) {
            s_player.active = false;
            buzzer_set(false, 0, 0);
            return false;
        }
        s_player.note_start_ms = now;
        note = &s_player.notes[s_player.idx];
        buzzer_set(note->freq_hz != 0, note->freq_hz, note->duty);
    }
    return true;
}

/* Is the current hint an "active game" hint (countdown running)? */
static bool hint_is_running(prop_led_hint_t h)
{
    return h == PROP_LED_HINT_COUNTDOWN;
}

/* Is it a terminal result state? */
static bool hint_is_result(prop_led_hint_t h)
{
    return h == PROP_LED_HINT_DEFUSED || h == PROP_LED_HINT_DETONATED;
}

static void buzzer_task(void *arg)
{
    (void)arg;

    prop_led_hint_t prev_hint = PROP_LED_HINT_OFF;
    prop_buzzer_mml_config_t buzzer_cfg;

    while (true) {
        prop_led_hint_t hint = prop_engine_get_led_hint();
        int64_t         t_ms = esp_timer_get_time() / 1000;

        /* --- detect transitions and trigger songs --- */
        if (hint != prev_hint) {
            prop_engine_get_buzzer_mml_config(&buzzer_cfg);

            if (hint == PROP_LED_HINT_COUNTDOWN) {
                /* Start or resume → up beep */
                buzzer_play_mml(buzzer_cfg.start_resume);
            } else if (hint == PROP_LED_HINT_PAUSED ||
                       (hint == PROP_LED_HINT_READY && hint_is_running(prev_hint)) ||
                       (hint == PROP_LED_HINT_NOT_READY && hint_is_running(prev_hint))) {
                /* Pause or reset during game → down beep */
                buzzer_play_mml(buzzer_cfg.pause_reset);
            } else if (hint == PROP_LED_HINT_DEFUSED) {
                buzzer_play_mml(buzzer_cfg.solved);
            } else if (hint == PROP_LED_HINT_DETONATED) {
                buzzer_play_mml(buzzer_cfg.failed);
            } else if (hint_is_result(prev_hint) && !hint_is_result(hint)) {
                /* Leaving a result state — silence anything still playing */
                s_player.active = false;
                buzzer_set(false, 0, 0);
            }
            prev_hint = hint;
        }

        /* --- run sequencer --- */
        if (buzzer_tick()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* --- ongoing tones when no song is queued --- */
        if (hint == PROP_LED_HINT_PENALTY) {
            bool on = ((t_ms % 400) < 80);
            buzzer_set(on, 1200, duty_from_volume(65));
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void init_buzzer(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ledc_channel_config_t channel_cfg = {
        .gpio_num = PIEZO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));

    ESP_LOGI(TAG, "Buzzer PWM initialized on GPIO %d", PIEZO_GPIO);

    buzzer_set(true, 1800, BUZZER_DUTY_50);
    vTaskDelay(pdMS_TO_TICKS(80));
    buzzer_set(false, 0, 0);
}

/* GPIO interrupt handler: wake from light sleep when a wire is disconnected */
static void IRAM_ATTR wire_gpio_isr_handler(void *arg)
{
    /* ISR just needs to wake the MCU; state change will be detected in the main tasks. */
    (void)arg;
}

/* Configure GPIO edge detection on wire inputs for light sleep wake */
static void init_wire_gpio_interrupts(void)
{
    /* Wire inputs are typically GPIO3-GPIO6 on ESP32-S3 DevKitC-1 */
    const int wire_gpios[] = {3, 4, 5, 6};
    int i;

    gpio_install_isr_service(0);
    
    for (i = 0; i < 4; ++i) {
        gpio_isr_handler_add(wire_gpios[i], wire_gpio_isr_handler, (void *)(intptr_t)i);
        /* Trigger on HIGH (wire disconnect = logic HIGH) */
        gpio_set_intr_type(wire_gpios[i], GPIO_INTR_POSEDGE);
        ESP_LOGI(TAG, "Configured GPIO %d for wire disconnect detection", wire_gpios[i]);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting px-wifi-v1");
    log_boot_wakeup_info();

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

    init_buzzer();
    xTaskCreate(buzzer_task, "buzzer_status", 3072, NULL, 5, NULL);

    /* Start web/wifi first — its radio init causes a 5V power surge that can
     * disrupt I2C.  Initialise the display after wifi is up to avoid this. */
    ESP_ERROR_CHECK(web_ui_start());

    /* Light sleep is enabled automatically via FreeRTOS tickless idle —
     * no need for explicit esp_pm_configure(). Tasks will sleep during vTaskDelay(). */
    
    init_wire_gpio_interrupts();

    init_display();
    if (s_display_ready) {
        xTaskCreate(display_task, "display_status", 4096, NULL, 5, NULL);
    }
}

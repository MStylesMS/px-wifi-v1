#include "board.h"
#include "px_system.h"
#include "drv_rgb_led.h"
#include "web_ui.h"
#include "prop_engine.h"
#include <ctype.h>
#include <math.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "px-wifi-v1";

#define PIEZO_GPIO      47
#define BUZZER_DUTY_50  512
#define BUZZER_MAX_NOTES 96

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

    init_buzzer();
    xTaskCreate(buzzer_task, "buzzer_status", 3072, NULL, 5, NULL);

    ESP_ERROR_CHECK(web_ui_start());
}

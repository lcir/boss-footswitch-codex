#include "buttons.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "buttons";

typedef struct {
    gpio_num_t gpio;
    app_button_id_t button_id;
} button_pin_map_t;

typedef struct {
    buttons_config_t config;
    TickType_t pressed_at[APP_BUTTON_COUNT];
    bool long_sent[APP_BUTTON_COUNT];
    bool pressed[APP_BUTTON_COUNT];
    bool combo_reset_sent;
} buttons_ctx_t;

static buttons_ctx_t s_ctx;

static const button_pin_map_t s_button_map[APP_BUTTON_COUNT] = {
    { APP_BUTTON_PRESET_A1_GPIO, APP_BUTTON_PRESET_A1 },
    { APP_BUTTON_PRESET_A2_GPIO, APP_BUTTON_PRESET_A2 },
    { APP_BUTTON_PRESET_B1_GPIO, APP_BUTTON_PRESET_B1 },
    { APP_BUTTON_PRESET_B2_GPIO, APP_BUTTON_PRESET_B2 },
    { APP_BUTTON_SOLO_GPIO, APP_BUTTON_SOLO },
    { APP_BUTTON_MODE_GPIO, APP_BUTTON_MODE },
};

static void emit_action(app_action_t action) {
    if (s_ctx.config.callback != NULL) {
        s_ctx.config.callback(&action, s_ctx.config.user_ctx);
    }
}

static void emit_press_action(app_button_id_t button_id) {
    app_action_t action = {0};
    app_state_snapshot_t snapshot;

    app_state_get(s_ctx.config.state, &snapshot);

    switch (button_id) {
        case APP_BUTTON_PRESET_A1:
            action.type = snapshot.footswitch_mode == APP_FOOTSWITCH_MODE_PRESET
                ? APP_ACTION_PRESET_SELECT
                : APP_ACTION_EFFECT_TOGGLE;
            if (snapshot.footswitch_mode == APP_FOOTSWITCH_MODE_PRESET) {
                action.value.preset = APP_PRESET_A1;
            } else {
                action.value.effect = APP_EFFECT_BOOSTER;
            }
            break;
        case APP_BUTTON_PRESET_A2:
            action.type = snapshot.footswitch_mode == APP_FOOTSWITCH_MODE_PRESET
                ? APP_ACTION_PRESET_SELECT
                : APP_ACTION_EFFECT_TOGGLE;
            if (snapshot.footswitch_mode == APP_FOOTSWITCH_MODE_PRESET) {
                action.value.preset = APP_PRESET_A2;
            } else {
                action.value.effect = APP_EFFECT_MOD;
            }
            break;
        case APP_BUTTON_PRESET_B1:
            action.type = snapshot.footswitch_mode == APP_FOOTSWITCH_MODE_PRESET
                ? APP_ACTION_PRESET_SELECT
                : APP_ACTION_EFFECT_TOGGLE;
            if (snapshot.footswitch_mode == APP_FOOTSWITCH_MODE_PRESET) {
                action.value.preset = APP_PRESET_B1;
            } else {
                action.value.effect = APP_EFFECT_FX;
            }
            break;
        case APP_BUTTON_PRESET_B2:
            action.type = snapshot.footswitch_mode == APP_FOOTSWITCH_MODE_PRESET
                ? APP_ACTION_PRESET_SELECT
                : APP_ACTION_EFFECT_TOGGLE;
            if (snapshot.footswitch_mode == APP_FOOTSWITCH_MODE_PRESET) {
                action.value.preset = APP_PRESET_B2;
            } else {
                action.value.effect = APP_EFFECT_DELAY;
            }
            break;
        case APP_BUTTON_SOLO:
            action.type = APP_ACTION_PANEL_SELECT;
            break;
        case APP_BUTTON_MODE:
            action.type = APP_ACTION_MODE_SET;
            action.value.mode = APP_FOOTSWITCH_MODE_EFFECT;
            break;
    }

    emit_action(action);
}

static void handle_long_press(app_button_id_t button_id) {
    app_action_t action = {0};

    if (button_id == APP_BUTTON_MODE) {
        action.type = APP_ACTION_MODE_SET;
        action.value.mode = APP_FOOTSWITCH_MODE_PRESET;
        emit_action(action);
    }
}

static void buttons_task(void *unused) {
    (void) unused;

    while (true) {
        const TickType_t now = xTaskGetTickCount();

        if (s_ctx.pressed[APP_BUTTON_MODE] && s_ctx.pressed[APP_BUTTON_SOLO] && !s_ctx.combo_reset_sent) {
            const TickType_t mode_held_ms = pdTICKS_TO_MS(now - s_ctx.pressed_at[APP_BUTTON_MODE]);
            const TickType_t solo_held_ms = pdTICKS_TO_MS(now - s_ctx.pressed_at[APP_BUTTON_SOLO]);
            if (mode_held_ms >= APP_RESET_HOLD_MS && solo_held_ms >= APP_RESET_HOLD_MS) {
                s_ctx.combo_reset_sent = true;
                s_ctx.long_sent[APP_BUTTON_MODE] = true;
                s_ctx.long_sent[APP_BUTTON_SOLO] = true;
                emit_action((app_action_t){ .type = APP_ACTION_WIFI_RESET });
            }
        }

        for (size_t i = 0; i < APP_BUTTON_COUNT; ++i) {
            const bool is_down = gpio_get_level(s_button_map[i].gpio) == 0;

            if (is_down && !s_ctx.pressed[i]) {
                s_ctx.pressed[i] = true;
                s_ctx.long_sent[i] = false;
                s_ctx.pressed_at[i] = now;
            } else if (!is_down && s_ctx.pressed[i]) {
                const TickType_t held_ms = pdTICKS_TO_MS(now - s_ctx.pressed_at[i]);
                s_ctx.pressed[i] = false;

                if (!s_ctx.long_sent[i] && held_ms >= APP_DEBOUNCE_MS && !s_ctx.combo_reset_sent) {
                    emit_press_action(s_button_map[i].button_id);
                }
            } else if (is_down && s_ctx.pressed[i] && !s_ctx.long_sent[i]) {
                const TickType_t held_ms = pdTICKS_TO_MS(now - s_ctx.pressed_at[i]);
                if (held_ms >= APP_LONG_PRESS_MS) {
                    s_ctx.long_sent[i] = true;
                    handle_long_press(s_button_map[i].button_id);
                }
            }
        }

        if (s_ctx.combo_reset_sent && !s_ctx.pressed[APP_BUTTON_MODE] && !s_ctx.pressed[APP_BUTTON_SOLO]) {
            s_ctx.combo_reset_sent = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t buttons_init(const buttons_config_t *config) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.config = *config;

    for (size_t i = 0; i < APP_BUTTON_COUNT; ++i) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << s_button_map[i].gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf));
    }

    xTaskCreate(buttons_task, "buttons", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "Buttons initialized");
    return ESP_OK;
}

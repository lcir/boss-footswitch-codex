#include "leds.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"

#include "app_config.h"

static const char *TAG = "leds";

static leds_rgb_t scale(leds_rgb_t color, uint8_t brightness) {
    color.red = (uint8_t) ((color.red * brightness) / 255);
    color.green = (uint8_t) ((color.green * brightness) / 255);
    color.blue = (uint8_t) ((color.blue * brightness) / 255);
    return color;
}

static leds_rgb_t preset_color(bool active) {
    return active ? (leds_rgb_t){ .red = 0, .green = 180, .blue = 255 } : (leds_rgb_t){ .red = 0, .green = 20, .blue = 36 };
}

static leds_rgb_t effect_color(bool active) {
    return active ? (leds_rgb_t){ .red = 255, .green = 120, .blue = 0 } : (leds_rgb_t){ .red = 40, .green = 18, .blue = 0 };
}

static leds_rgb_t transport_color(const app_state_snapshot_t *snapshot) {
    if (snapshot->runtime_state == APP_RUNTIME_ERROR) {
        return (leds_rgb_t){ .red = 255, .green = 0, .blue = 0 };
    }
    if (snapshot->ble_state != APP_BLE_CONNECTED) {
        return (leds_rgb_t){ .red = 255, .green = 120, .blue = 0 };
    }
    return snapshot->footswitch_mode == APP_FOOTSWITCH_MODE_PRESET
        ? (leds_rgb_t){ .red = 80, .green = 80, .blue = 80 }
        : (leds_rgb_t){ .red = 0, .green = 0, .blue = 255 };
}

static void leds_write_stub(const leds_rgb_t *pixels, size_t pixel_count) {
    char buffer[192] = {0};
    int offset = 0;

    for (size_t i = 0; i < pixel_count && offset < (int) sizeof(buffer) - 20; ++i) {
        offset += snprintf(
            buffer + offset,
            sizeof(buffer) - (size_t) offset,
            "[%u,%u,%u]",
            pixels[i].red,
            pixels[i].green,
            pixels[i].blue);
    }

    ESP_LOGD(TAG, "LED frame %s", buffer);
}

esp_err_t leds_init(void) {
    gpio_config_t conf = {
        .pin_bit_mask = 1ULL << APP_LEVEL_SHIFTER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&conf));
    gpio_set_level(APP_LEVEL_SHIFTER_GPIO, 1);
    ESP_LOGI(TAG, "LED pipeline initialized in stub mode on GPIO %d", APP_LED_DATA_GPIO);
    return ESP_OK;
}

esp_err_t leds_render_state(const app_state_snapshot_t *snapshot) {
    leds_rgb_t pixels[APP_LED_COUNT];
    memset(pixels, 0, sizeof(pixels));

    if (snapshot->footswitch_mode == APP_FOOTSWITCH_MODE_PRESET) {
        pixels[0] = preset_color(snapshot->active_preset == APP_PRESET_A1);
        pixels[1] = preset_color(snapshot->active_preset == APP_PRESET_A2);
        pixels[2] = preset_color(snapshot->active_preset == APP_PRESET_B1);
        pixels[3] = preset_color(snapshot->active_preset == APP_PRESET_B2);
    } else {
        pixels[0] = effect_color(snapshot->effects[APP_EFFECT_BOOSTER]);
        pixels[1] = effect_color(snapshot->effects[APP_EFFECT_MOD]);
        pixels[2] = effect_color(snapshot->effects[APP_EFFECT_FX]);
        pixels[3] = effect_color(snapshot->effects[APP_EFFECT_DELAY]);
    }

    pixels[4] = snapshot->solo_enabled
        ? (leds_rgb_t){ .red = 255, .green = 0, .blue = 0 }
        : (leds_rgb_t){ .red = 40, .green = 0, .blue = 0 };
    pixels[5] = transport_color(snapshot);

    for (size_t i = 0; i < APP_LED_COUNT; ++i) {
        pixels[i] = scale(pixels[i], snapshot->led_brightness);
    }

    leds_write_stub(pixels, APP_LED_COUNT);
    return ESP_OK;
}

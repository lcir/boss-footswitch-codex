#pragma once

#include "esp_err.h"

#include "app_state.h"

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} leds_rgb_t;

esp_err_t leds_init(void);
esp_err_t leds_render_state(const app_state_snapshot_t *snapshot);

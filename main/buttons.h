#pragma once

#include "esp_err.h"

#include "app_state.h"

typedef void (*buttons_action_callback_t)(const app_action_t *action, void *user_ctx);

typedef struct {
    app_state_store_t *state;
    buttons_action_callback_t callback;
    void *user_ctx;
} buttons_config_t;

esp_err_t buttons_init(const buttons_config_t *config);

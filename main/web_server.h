#pragma once

#include "esp_err.h"

#include "amp_transport.h"
#include "app_state.h"
#include "midi_config.h"

typedef void (*web_action_callback_t)(const app_action_t *action, void *user_ctx);

typedef struct {
    app_state_store_t *state;
    midi_config_store_t *midi_config;
    amp_transport_t *transport;
    web_action_callback_t on_action;
    void *user_ctx;
} web_server_config_t;

esp_err_t web_server_init(const web_server_config_t *config);
esp_err_t web_server_start(void);
esp_err_t web_server_broadcast_state(const app_state_snapshot_t *snapshot);

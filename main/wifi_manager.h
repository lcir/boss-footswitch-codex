#pragma once

#include "esp_err.h"

#include "app_state.h"

typedef void (*wifi_reconfigure_callback_t)(void *user_ctx);

typedef struct {
    app_state_store_t *state;
    wifi_reconfigure_callback_t on_wifi_ready;
    void *user_ctx;
} wifi_manager_config_t;

esp_err_t wifi_manager_init(const wifi_manager_config_t *config);
bool wifi_manager_is_provisioned(void);
esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_reset_credentials(void);

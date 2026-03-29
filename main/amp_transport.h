#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_state.h"
#include "midi_config.h"

typedef struct {
    app_state_store_t *state;
    midi_config_store_t *midi_config;
} amp_transport_config_t;

typedef struct {
    app_state_store_t *state;
    midi_config_store_t *midi_config;
    SemaphoreHandle_t mutex;
    bool host_ready;
    bool connect_requested;
    bool scan_active;
    bool connect_in_progress;
    bool connected;
    bool security_in_progress;
    bool link_encrypted;
    bool gatt_ready;
    bool subscription_active;
    uint8_t own_addr_type;
    uint16_t conn_handle;
    uint16_t midi_service_start_handle;
    uint16_t midi_service_end_handle;
    uint16_t midi_char_val_handle;
    uint16_t midi_cccd_handle;
    uint8_t midi_char_properties;
} amp_transport_t;

esp_err_t amp_transport_init(amp_transport_t *transport, const amp_transport_config_t *config);
esp_err_t amp_transport_connect(amp_transport_t *transport);
esp_err_t amp_transport_disconnect(amp_transport_t *transport);
esp_err_t amp_transport_poll_read(amp_transport_t *transport);
bool amp_transport_should_apply_optimistic_action(const amp_transport_t *transport, const app_action_t *action);
esp_err_t amp_transport_dispatch_action(amp_transport_t *transport, const app_action_t *action);
esp_err_t amp_transport_resync(amp_transport_t *transport);
esp_err_t amp_transport_run_pc_offset_test(
    amp_transport_t *transport,
    app_preset_id_t preset,
    app_pc_offset_mode_t pc_offset_mode);

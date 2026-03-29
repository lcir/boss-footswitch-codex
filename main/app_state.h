#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"

typedef enum {
    APP_BLE_DISCONNECTED = 0,
    APP_BLE_SCANNING,
    APP_BLE_CONNECTING,
    APP_BLE_CONNECTED,
    APP_BLE_ERROR,
} app_ble_state_t;

typedef enum {
    APP_WIFI_UNPROVISIONED = 0,
    APP_WIFI_AP_MODE,
    APP_WIFI_CONNECTING,
    APP_WIFI_CONNECTED,
    APP_WIFI_ERROR,
} app_wifi_state_t;

typedef struct {
    bool connected;
    char ssid[33];
    char ip[16];
    char hostname[32];
} app_wifi_snapshot_t;

typedef struct {
    app_runtime_state_t runtime_state;
    app_footswitch_mode_t footswitch_mode;
    app_ble_state_t ble_state;
    app_wifi_state_t wifi_state;
    app_pc_offset_mode_t pc_offset_mode;
    app_amp_state_confidence_t amp_state_confidence;
    app_preset_id_t active_preset;
    bool solo_enabled;
    bool effects[APP_EFFECT_COUNT];
    bool synced;
    bool midi_configured;
    bool solo_configured;
    bool wifi_provisioned;
    bool captive_portal_active;
    uint8_t led_brightness;
    uint32_t state_version;
    app_wifi_snapshot_t wifi;
    char last_error[96];
} app_state_snapshot_t;

typedef struct {
    SemaphoreHandle_t mutex;
    app_state_snapshot_t snapshot;
} app_state_store_t;

typedef enum {
    APP_ACTION_PRESET_SELECT = 0,
    APP_ACTION_PANEL_SELECT,
    APP_ACTION_MODE_SET,
    APP_ACTION_EFFECT_TOGGLE,
    APP_ACTION_BLE_RECONNECT,
    APP_ACTION_WIFI_RESET,
    APP_ACTION_RESYNC,
} app_action_type_t;

typedef struct {
    app_action_type_t type;
    union {
        app_preset_id_t preset;
        app_footswitch_mode_t mode;
        app_effect_id_t effect;
    } value;
} app_action_t;

void app_state_init(app_state_store_t *store);
void app_state_get(const app_state_store_t *store, app_state_snapshot_t *out_snapshot);
void app_state_set_runtime(app_state_store_t *store, app_runtime_state_t runtime_state);
void app_state_set_ble(app_state_store_t *store, app_ble_state_t ble_state);
void app_state_set_wifi_mode(app_state_store_t *store, app_wifi_state_t wifi_state);
void app_state_set_wifi_snapshot(app_state_store_t *store, const app_wifi_snapshot_t *wifi_snapshot);
void app_state_set_provisioned(app_state_store_t *store, bool provisioned, bool captive_portal_active);
void app_state_set_midi_status(
    app_state_store_t *store,
    bool midi_configured,
    bool solo_configured,
    app_pc_offset_mode_t pc_offset_mode);
void app_state_set_amp_state_confidence(
    app_state_store_t *store,
    app_amp_state_confidence_t confidence);
void app_state_set_error(app_state_store_t *store, const char *message);
void app_state_clear_error(app_state_store_t *store);
void app_state_apply_optimistic_action(app_state_store_t *store, const app_action_t *action);
void app_state_set_remote_preset(app_state_store_t *store, app_preset_id_t preset);
void app_state_mark_synced(app_state_store_t *store, bool synced);
void app_state_set_led_brightness(app_state_store_t *store, uint8_t led_brightness);

#include "app_state.h"

#include <string.h>

#include "esp_check.h"

static void app_state_lock(const app_state_store_t *store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
}

static void app_state_unlock(const app_state_store_t *store) {
    xSemaphoreGive(store->mutex);
}

void app_state_init(app_state_store_t *store) {
    memset(store, 0, sizeof(*store));
    store->mutex = xSemaphoreCreateMutex();
    store->snapshot.runtime_state = APP_RUNTIME_BOOT;
    store->snapshot.footswitch_mode = APP_FOOTSWITCH_MODE_PRESET;
    store->snapshot.ble_state = APP_BLE_DISCONNECTED;
    store->snapshot.wifi_state = APP_WIFI_UNPROVISIONED;
    store->snapshot.pc_offset_mode = APP_PC_OFFSET_UNKNOWN;
    store->snapshot.amp_state_confidence = APP_AMP_STATE_CONFIDENCE_NONE;
    store->snapshot.active_preset = APP_PRESET_A1;
    store->snapshot.led_brightness = 32;
    strncpy(store->snapshot.wifi.hostname, APP_HOSTNAME, sizeof(store->snapshot.wifi.hostname) - 1);
}

void app_state_get(const app_state_store_t *store, app_state_snapshot_t *out_snapshot) {
    app_state_lock(store);
    memcpy(out_snapshot, &store->snapshot, sizeof(*out_snapshot));
    app_state_unlock(store);
}

void app_state_set_runtime(app_state_store_t *store, app_runtime_state_t runtime_state) {
    app_state_lock(store);
    store->snapshot.runtime_state = runtime_state;
    store->snapshot.state_version++;
    app_state_unlock(store);
}

void app_state_set_ble(app_state_store_t *store, app_ble_state_t ble_state) {
    app_state_lock(store);
    store->snapshot.ble_state = ble_state;
    if (ble_state == APP_BLE_CONNECTED && store->snapshot.runtime_state != APP_RUNTIME_ERROR) {
        store->snapshot.runtime_state = store->snapshot.footswitch_mode == APP_FOOTSWITCH_MODE_PRESET
            ? APP_RUNTIME_PRESET
            : APP_RUNTIME_EFFECT;
    } else if (ble_state == APP_BLE_CONNECTING || ble_state == APP_BLE_SCANNING) {
        store->snapshot.runtime_state = APP_RUNTIME_BLE_CONNECTING;
    } else if (ble_state == APP_BLE_DISCONNECTED || ble_state == APP_BLE_ERROR) {
        store->snapshot.amp_state_confidence = APP_AMP_STATE_CONFIDENCE_NONE;
    }
    store->snapshot.state_version++;
    app_state_unlock(store);
}

void app_state_set_wifi_mode(app_state_store_t *store, app_wifi_state_t wifi_state) {
    app_state_lock(store);
    store->snapshot.wifi_state = wifi_state;
    store->snapshot.state_version++;
    app_state_unlock(store);
}

void app_state_set_wifi_snapshot(app_state_store_t *store, const app_wifi_snapshot_t *wifi_snapshot) {
    app_state_lock(store);
    store->snapshot.wifi = *wifi_snapshot;
    store->snapshot.state_version++;
    app_state_unlock(store);
}

void app_state_set_provisioned(app_state_store_t *store, bool provisioned, bool captive_portal_active) {
    app_state_lock(store);
    store->snapshot.wifi_provisioned = provisioned;
    store->snapshot.captive_portal_active = captive_portal_active;
    store->snapshot.runtime_state = provisioned ? APP_RUNTIME_BLE_CONNECTING : APP_RUNTIME_WIFI_PROVISION;
    store->snapshot.state_version++;
    app_state_unlock(store);
}

void app_state_set_midi_status(
    app_state_store_t *store,
    bool midi_configured,
    bool solo_configured,
    app_pc_offset_mode_t pc_offset_mode) {
    app_state_lock(store);
    store->snapshot.midi_configured = midi_configured;
    store->snapshot.solo_configured = solo_configured;
    store->snapshot.pc_offset_mode = pc_offset_mode;
    store->snapshot.state_version++;
    app_state_unlock(store);
}

void app_state_set_amp_state_confidence(
    app_state_store_t *store,
    app_amp_state_confidence_t confidence) {
    app_state_lock(store);
    store->snapshot.amp_state_confidence = confidence;
    store->snapshot.state_version++;
    app_state_unlock(store);
}

void app_state_set_error(app_state_store_t *store, const char *message) {
    app_state_lock(store);
    store->snapshot.runtime_state = APP_RUNTIME_ERROR;
    store->snapshot.ble_state = APP_BLE_ERROR;
    store->snapshot.wifi_state = APP_WIFI_ERROR;
    strlcpy(store->snapshot.last_error, message, sizeof(store->snapshot.last_error));
    store->snapshot.state_version++;
    app_state_unlock(store);
}

void app_state_clear_error(app_state_store_t *store) {
    app_state_lock(store);
    memset(store->snapshot.last_error, 0, sizeof(store->snapshot.last_error));
    if (store->snapshot.ble_state == APP_BLE_ERROR) {
        store->snapshot.ble_state = APP_BLE_DISCONNECTED;
    }
    if (store->snapshot.wifi_state == APP_WIFI_ERROR) {
        store->snapshot.wifi_state = store->snapshot.wifi_provisioned ? APP_WIFI_CONNECTING : APP_WIFI_UNPROVISIONED;
    }
    store->snapshot.amp_state_confidence = APP_AMP_STATE_CONFIDENCE_NONE;
    store->snapshot.runtime_state = store->snapshot.wifi_provisioned
        ? APP_RUNTIME_BLE_CONNECTING
        : APP_RUNTIME_WIFI_PROVISION;
    store->snapshot.state_version++;
    app_state_unlock(store);
}

void app_state_apply_optimistic_action(app_state_store_t *store, const app_action_t *action) {
    app_state_lock(store);
    switch (action->type) {
        case APP_ACTION_PRESET_SELECT:
            store->snapshot.active_preset = action->value.preset;
            store->snapshot.footswitch_mode = APP_FOOTSWITCH_MODE_PRESET;
            store->snapshot.runtime_state = APP_RUNTIME_PRESET;
            store->snapshot.amp_state_confidence = APP_AMP_STATE_CONFIDENCE_LOW;
            break;
        case APP_ACTION_SOLO_TOGGLE:
            store->snapshot.solo_enabled = !store->snapshot.solo_enabled;
            store->snapshot.amp_state_confidence = APP_AMP_STATE_CONFIDENCE_LOW;
            break;
        case APP_ACTION_MODE_SET:
            store->snapshot.footswitch_mode = action->value.mode;
            store->snapshot.runtime_state = action->value.mode == APP_FOOTSWITCH_MODE_PRESET
                ? APP_RUNTIME_PRESET
                : APP_RUNTIME_EFFECT;
            break;
        case APP_ACTION_EFFECT_TOGGLE:
            if (action->value.effect < APP_EFFECT_COUNT) {
                store->snapshot.effects[action->value.effect] = !store->snapshot.effects[action->value.effect];
                store->snapshot.amp_state_confidence = APP_AMP_STATE_CONFIDENCE_LOW;
            }
            break;
        case APP_ACTION_BLE_RECONNECT:
            store->snapshot.ble_state = APP_BLE_CONNECTING;
            store->snapshot.runtime_state = APP_RUNTIME_BLE_CONNECTING;
            store->snapshot.amp_state_confidence = APP_AMP_STATE_CONFIDENCE_NONE;
            break;
        case APP_ACTION_WIFI_RESET:
            store->snapshot.wifi_provisioned = false;
            store->snapshot.captive_portal_active = true;
            store->snapshot.wifi_state = APP_WIFI_AP_MODE;
            store->snapshot.runtime_state = APP_RUNTIME_WIFI_PROVISION;
            store->snapshot.amp_state_confidence = APP_AMP_STATE_CONFIDENCE_NONE;
            memset(&store->snapshot.wifi, 0, sizeof(store->snapshot.wifi));
            break;
        case APP_ACTION_RESYNC:
            store->snapshot.amp_state_confidence = APP_AMP_STATE_CONFIDENCE_NONE;
            break;
    }
    store->snapshot.synced = false;
    store->snapshot.state_version++;
    app_state_unlock(store);
}

void app_state_set_remote_preset(app_state_store_t *store, app_preset_id_t preset) {
    app_state_lock(store);
    store->snapshot.active_preset = preset;
    store->snapshot.footswitch_mode = APP_FOOTSWITCH_MODE_PRESET;
    if (store->snapshot.ble_state == APP_BLE_CONNECTED && store->snapshot.runtime_state != APP_RUNTIME_ERROR) {
        store->snapshot.runtime_state = APP_RUNTIME_PRESET;
    }
    store->snapshot.synced = true;
    store->snapshot.amp_state_confidence = APP_AMP_STATE_CONFIDENCE_HIGH;
    store->snapshot.state_version++;
    app_state_unlock(store);
}

void app_state_mark_synced(app_state_store_t *store, bool synced) {
    app_state_lock(store);
    store->snapshot.synced = synced;
    store->snapshot.state_version++;
    app_state_unlock(store);
}

void app_state_set_led_brightness(app_state_store_t *store, uint8_t led_brightness) {
    app_state_lock(store);
    store->snapshot.led_brightness = led_brightness;
    store->snapshot.state_version++;
    app_state_unlock(store);
}

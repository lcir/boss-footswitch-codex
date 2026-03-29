#include "midi_config.h"

#include <string.h>

#include "esp_check.h"
#include "nvs.h"

static const char *TAG = "midi_config";
static const char *NVS_NAMESPACE = "midi";

static void midi_config_lock(const midi_config_store_t *store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
}

static void midi_config_unlock(const midi_config_store_t *store) {
    xSemaphoreGive(store->mutex);
}

static void midi_config_set_defaults(midi_config_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->pc_panel = 5;
    snapshot->pc_a1 = 1;
    snapshot->pc_a2 = 2;
    snapshot->pc_b1 = 6;
    snapshot->pc_b2 = 7;
    snapshot->cc_booster = 16;
    snapshot->cc_mod = 17;
    snapshot->cc_fx = 18;
    snapshot->cc_delay = 19;
    snapshot->cc_reverb = 20;
    snapshot->cc_send_return = 21;
    snapshot->solo_configured = false;
    snapshot->cc_solo = 0;
    snapshot->pc_offset_mode = APP_PC_OFFSET_UNKNOWN;
}

static void load_u8_if_present(nvs_handle_t handle, const char *key, uint8_t *value) {
    uint8_t loaded = 0;
    if (nvs_get_u8(handle, key, &loaded) == ESP_OK) {
        *value = loaded;
    }
}

static void load_u32_if_present(nvs_handle_t handle, const char *key, uint32_t *value) {
    uint32_t loaded = 0;
    if (nvs_get_u32(handle, key, &loaded) == ESP_OK) {
        *value = loaded;
    }
}

esp_err_t midi_config_init(midi_config_store_t *store) {
    memset(store, 0, sizeof(*store));
    store->mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(store->mutex != NULL, ESP_ERR_NO_MEM, TAG, "create midi config mutex");
    midi_config_set_defaults(&store->snapshot);
    return midi_config_load(store);
}

esp_err_t midi_config_load(midi_config_store_t *store) {
    midi_config_snapshot_t loaded;
    midi_config_set_defaults(&loaded);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        midi_config_lock(store);
        store->snapshot = loaded;
        midi_config_unlock(store);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open midi config namespace");

    load_u8_if_present(handle, "pc_panel", &loaded.pc_panel);
    load_u8_if_present(handle, "pc_a1", &loaded.pc_a1);
    load_u8_if_present(handle, "pc_a2", &loaded.pc_a2);
    load_u8_if_present(handle, "pc_b1", &loaded.pc_b1);
    load_u8_if_present(handle, "pc_b2", &loaded.pc_b2);
    load_u8_if_present(handle, "cc_boost", &loaded.cc_booster);
    load_u8_if_present(handle, "cc_mod", &loaded.cc_mod);
    load_u8_if_present(handle, "cc_fx", &loaded.cc_fx);
    load_u8_if_present(handle, "cc_delay", &loaded.cc_delay);
    load_u8_if_present(handle, "cc_reverb", &loaded.cc_reverb);
    load_u8_if_present(handle, "cc_sendret", &loaded.cc_send_return);
    load_u8_if_present(handle, "cc_solo", &loaded.cc_solo);

    uint8_t solo_configured = loaded.solo_configured ? 1 : 0;
    load_u8_if_present(handle, "solo_cfg", &solo_configured);
    loaded.solo_configured = solo_configured != 0;

    uint32_t pc_offset_mode = (uint32_t) loaded.pc_offset_mode;
    load_u32_if_present(handle, "pc_offset", &pc_offset_mode);
    if (pc_offset_mode <= APP_PC_OFFSET_DIRECT) {
        loaded.pc_offset_mode = (app_pc_offset_mode_t) pc_offset_mode;
    }

    nvs_close(handle);

    midi_config_lock(store);
    store->snapshot = loaded;
    midi_config_unlock(store);
    return ESP_OK;
}

esp_err_t midi_config_save(midi_config_store_t *store, const midi_config_snapshot_t *snapshot) {
    nvs_handle_t handle;
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open midi config namespace");

    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "pc_panel", snapshot->pc_panel), exit, TAG, "save pc_panel");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "pc_a1", snapshot->pc_a1), exit, TAG, "save pc_a1");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "pc_a2", snapshot->pc_a2), exit, TAG, "save pc_a2");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "pc_b1", snapshot->pc_b1), exit, TAG, "save pc_b1");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "pc_b2", snapshot->pc_b2), exit, TAG, "save pc_b2");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "cc_boost", snapshot->cc_booster), exit, TAG, "save cc_booster");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "cc_mod", snapshot->cc_mod), exit, TAG, "save cc_mod");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "cc_fx", snapshot->cc_fx), exit, TAG, "save cc_fx");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "cc_delay", snapshot->cc_delay), exit, TAG, "save cc_delay");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "cc_reverb", snapshot->cc_reverb), exit, TAG, "save cc_reverb");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "cc_sendret", snapshot->cc_send_return), exit, TAG, "save cc_send_return");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "cc_solo", snapshot->cc_solo), exit, TAG, "save cc_solo");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "solo_cfg", snapshot->solo_configured ? 1 : 0), exit, TAG, "save solo_cfg");
    ESP_GOTO_ON_ERROR(nvs_set_u32(handle, "pc_offset", (uint32_t) snapshot->pc_offset_mode), exit, TAG, "save pc_offset");
    ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "commit midi config");

    midi_config_lock(store);
    store->snapshot = *snapshot;
    midi_config_unlock(store);

exit:
    nvs_close(handle);
    return ret;
}

void midi_config_get(const midi_config_store_t *store, midi_config_snapshot_t *out_snapshot) {
    midi_config_lock(store);
    *out_snapshot = store->snapshot;
    midi_config_unlock(store);
}

bool midi_config_is_ready(const midi_config_snapshot_t *snapshot) {
    return snapshot->pc_offset_mode != APP_PC_OFFSET_UNKNOWN;
}

bool midi_config_get_program_for_preset(
    const midi_config_snapshot_t *snapshot,
    app_preset_id_t preset,
    uint8_t *out_program) {
    if (!midi_config_is_ready(snapshot)) {
        return false;
    }

    uint8_t visible_program = 0;
    switch (preset) {
        case APP_PRESET_A1: visible_program = snapshot->pc_a1; break;
        case APP_PRESET_A2: visible_program = snapshot->pc_a2; break;
        case APP_PRESET_B1: visible_program = snapshot->pc_b1; break;
        case APP_PRESET_B2: visible_program = snapshot->pc_b2; break;
    }

    int wire_program = visible_program;
    if (snapshot->pc_offset_mode == APP_PC_OFFSET_SUBTRACT_ONE) {
        wire_program -= 1;
    }

    if (wire_program < 0 || wire_program > 127) {
        return false;
    }

    *out_program = (uint8_t) wire_program;
    return true;
}

bool midi_config_get_program_for_panel(
    const midi_config_snapshot_t *snapshot,
    uint8_t *out_program) {
    if (!midi_config_is_ready(snapshot)) {
        return false;
    }

    int wire_program = snapshot->pc_panel;
    if (snapshot->pc_offset_mode == APP_PC_OFFSET_SUBTRACT_ONE) {
        wire_program -= 1;
    }

    if (wire_program < 0 || wire_program > 127) {
        return false;
    }

    *out_program = (uint8_t) wire_program;
    return true;
}

bool midi_config_get_cc_for_effect(
    const midi_config_snapshot_t *snapshot,
    app_effect_id_t effect,
    uint8_t *out_cc) {
    switch (effect) {
        case APP_EFFECT_BOOSTER: *out_cc = snapshot->cc_booster; return true;
        case APP_EFFECT_MOD: *out_cc = snapshot->cc_mod; return true;
        case APP_EFFECT_FX: *out_cc = snapshot->cc_fx; return true;
        case APP_EFFECT_DELAY: *out_cc = snapshot->cc_delay; return true;
        case APP_EFFECT_REVERB: *out_cc = snapshot->cc_reverb; return true;
        case APP_EFFECT_COUNT: return false;
    }

    return false;
}

bool midi_config_get_cc_for_solo(
    const midi_config_snapshot_t *snapshot,
    uint8_t *out_cc) {
    if (!snapshot->solo_configured) {
        return false;
    }

    *out_cc = snapshot->cc_solo;
    return true;
}

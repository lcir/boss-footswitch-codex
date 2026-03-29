#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"

typedef struct {
    uint8_t pc_panel;
    uint8_t pc_a1;
    uint8_t pc_a2;
    uint8_t pc_b1;
    uint8_t pc_b2;
    uint8_t cc_booster;
    uint8_t cc_mod;
    uint8_t cc_fx;
    uint8_t cc_delay;
    uint8_t cc_reverb;
    uint8_t cc_send_return;
    bool solo_configured;
    uint8_t cc_solo;
    app_pc_offset_mode_t pc_offset_mode;
} midi_config_snapshot_t;

typedef struct {
    SemaphoreHandle_t mutex;
    midi_config_snapshot_t snapshot;
} midi_config_store_t;

esp_err_t midi_config_init(midi_config_store_t *store);
esp_err_t midi_config_load(midi_config_store_t *store);
esp_err_t midi_config_save(midi_config_store_t *store, const midi_config_snapshot_t *snapshot);
void midi_config_get(const midi_config_store_t *store, midi_config_snapshot_t *out_snapshot);
bool midi_config_is_ready(const midi_config_snapshot_t *snapshot);
bool midi_config_get_program_for_preset(
    const midi_config_snapshot_t *snapshot,
    app_preset_id_t preset,
    uint8_t *out_program);
bool midi_config_get_cc_for_effect(
    const midi_config_snapshot_t *snapshot,
    app_effect_id_t effect,
    uint8_t *out_cc);
bool midi_config_get_cc_for_solo(
    const midi_config_snapshot_t *snapshot,
    uint8_t *out_cc);

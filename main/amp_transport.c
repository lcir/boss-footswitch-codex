#include "amp_transport.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "store/config/ble_store_config.h"

#include "diag_log.h"

static const char *TAG = "amp_transport";
static const char *KATANA_MIDI_DEVICE_NAME = "KATANA 3 MIDI";
static const uint8_t MIDI_CHANNEL = 0;
static const uint8_t MIDI_SWITCH_ON = 127;
static const uint8_t MIDI_SWITCH_OFF = 0;

static const ble_uuid128_t s_midi_service_uuid = BLE_UUID128_INIT(
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7,
    0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);
static const ble_uuid128_t s_midi_char_uuid = BLE_UUID128_INIT(
    0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1,
    0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);

static amp_transport_t *s_transport;

void ble_store_config_init(void);

static void amp_transport_start_scan(amp_transport_t *transport);
static int amp_transport_gap_event(struct ble_gap_event *event, void *arg);
static int amp_transport_on_mtu_exchanged(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    uint16_t mtu,
    void *arg);

static void amp_transport_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void amp_transport_lock(amp_transport_t *transport) {
    xSemaphoreTake(transport->mutex, portMAX_DELAY);
}

static void amp_transport_unlock(amp_transport_t *transport) {
    xSemaphoreGive(transport->mutex);
}

static void format_ble_addr(const ble_addr_t *addr, char *buffer, size_t size) {
    snprintf(
        buffer,
        size,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        addr->val[5],
        addr->val[4],
        addr->val[3],
        addr->val[2],
        addr->val[1],
        addr->val[0]);
}

static void log_payload_hex(const char *prefix, const uint8_t *data, size_t len) {
    char line[192];
    size_t offset = 0;
    const size_t limit = len > 32 ? 32 : len;

    offset += snprintf(line + offset, sizeof(line) - offset, "%s", prefix);
    for (size_t i = 0; i < limit && offset + 4 < sizeof(line); ++i) {
        offset += snprintf(line + offset, sizeof(line) - offset, "%02X ", data[i]);
    }
    if (limit < len && offset + 4 < sizeof(line)) {
        snprintf(line + offset, sizeof(line) - offset, "...");
    }

    ESP_LOGI(TAG, "%s", line);
}

static bool adv_name_matches(const struct ble_hs_adv_fields *fields) {
    const size_t expected_len = strlen(KATANA_MIDI_DEVICE_NAME);
    return fields->name != NULL
        && fields->name_len == expected_len
        && memcmp(fields->name, KATANA_MIDI_DEVICE_NAME, expected_len) == 0;
}

static void amp_transport_reset_protocol_state(amp_transport_t *transport) {
    transport->midi_service_start_handle = 0;
    transport->midi_service_end_handle = 0;
    transport->midi_char_val_handle = 0;
    transport->midi_cccd_handle = 0;
    transport->midi_char_properties = 0;
}

static int midi_data_length(uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0:
        case 0xD0:
            return 1;
        case 0x80:
        case 0x90:
        case 0xA0:
        case 0xB0:
        case 0xE0:
            return 2;
        default:
            return 0;
    }
}

static const char *effect_name(app_effect_id_t effect) {
    static const char *labels[] = { "Booster", "Mod", "FX", "Delay", "Reverb" };
    return labels[effect];
}

static void amp_transport_handle_midi_message(
    amp_transport_t *transport,
    uint8_t status,
    uint8_t data1,
    uint8_t data2,
    uint8_t data_len) {
    (void) transport;

    if ((status & 0xF0) == 0xC0 && data_len == 1) {
        ESP_LOGI(
            TAG,
            "Inbound Program Change ch=%u program=%u",
            status & 0x0F,
            data1);
        return;
    }

    if ((status & 0xF0) == 0xB0 && data_len == 2) {
        ESP_LOGI(
            TAG,
            "Inbound Control Change ch=%u cc=%u value=%u",
            status & 0x0F,
            data1,
            data2);
        return;
    }

    ESP_LOGI(
        TAG,
        "Inbound MIDI status=0x%02X data1=0x%02X data2=0x%02X len=%u",
        status,
        data1,
        data2,
        data_len);
}

static void amp_transport_parse_ble_midi_packet(
    amp_transport_t *transport,
    const uint8_t *data,
    size_t len) {
    uint8_t running_status = 0;
    size_t i = 1;

    if (len < 2) {
        return;
    }

    log_payload_hex("Inbound BLE-MIDI payload: ", data, len);

    while (i < len) {
        if ((data[i] & 0x80) == 0) {
            ++i;
            continue;
        }

        ++i;
        if (i >= len) {
            break;
        }

        uint8_t status = data[i];
        uint8_t data1 = 0;
        uint8_t data2 = 0;

        if (status & 0x80) {
            ++i;
            if (status >= 0xF8) {
                amp_transport_handle_midi_message(transport, status, 0, 0, 0);
                continue;
            }
            if (status >= 0xF0) {
                ESP_LOGI(TAG, "Ignoring inbound non-channel MIDI status=0x%02X", status);
                running_status = 0;
                continue;
            }
            running_status = status;
        } else if (running_status != 0) {
            status = running_status;
        } else {
            ++i;
            continue;
        }

        const int needed = midi_data_length(status);
        if (needed == 0) {
            continue;
        }

        if ((status & 0x80) == 0) {
            data1 = status;
        } else {
            if (i >= len || (data[i] & 0x80) != 0) {
                continue;
            }
            data1 = data[i++];
        }

        if (needed == 2) {
            if (i >= len || (data[i] & 0x80) != 0) {
                continue;
            }
            data2 = data[i++];
        }

        amp_transport_handle_midi_message(transport, status, data1, data2, (uint8_t) needed);
    }
}

static esp_err_t amp_transport_send_ble_midi(
    amp_transport_t *transport,
    const uint8_t *midi_bytes,
    size_t midi_len) {
    uint16_t conn_handle = 0;
    uint16_t value_handle = 0;

    amp_transport_lock(transport);
    const bool ready =
        transport->connected
        && transport->link_encrypted
        && transport->subscription_active
        && transport->midi_char_val_handle != 0;
    if (ready) {
        conn_handle = transport->conn_handle;
        value_handle = transport->midi_char_val_handle;
    }
    amp_transport_unlock(transport);

    if (!ready) {
        ESP_LOGW(TAG, "BLE-MIDI write skipped because transport is not ready");
        DIAG_LOGW("ble", "Skipped BLE-MIDI write because the transport is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t packet[8];
    if (midi_len + 2 > sizeof(packet)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t timestamp = (uint16_t) ((esp_timer_get_time() / 1000ULL) & 0x1FFF);
    packet[0] = 0x80 | ((timestamp >> 7) & 0x3F);
    packet[1] = 0x80 | (timestamp & 0x7F);
    memcpy(&packet[2], midi_bytes, midi_len);

    const int rc = ble_gattc_write_no_rsp_flat(conn_handle, value_handle, packet, midi_len + 2);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE-MIDI write failed; rc=%d", rc);
        DIAG_LOGE("ble", "BLE-MIDI write failed rc=%d", rc);
        return ESP_FAIL;
    }

    log_payload_hex("Outbound BLE-MIDI payload: ", packet, midi_len + 2);
    return ESP_OK;
}

static esp_err_t amp_transport_send_program_change(amp_transport_t *transport, uint8_t program) {
    const uint8_t message[] = { (uint8_t) (0xC0 | MIDI_CHANNEL), program };
    ESP_LOGI(TAG, "Sending Program Change program=%u", program);
    DIAG_LOGI("ble", "Sending Program Change %u", program);
    return amp_transport_send_ble_midi(transport, message, sizeof(message));
}

static esp_err_t amp_transport_send_control_change(
    amp_transport_t *transport,
    uint8_t controller,
    uint8_t value) {
    const uint8_t message[] = { (uint8_t) (0xB0 | MIDI_CHANNEL), controller, value };
    ESP_LOGI(TAG, "Sending Control Change cc=%u value=%u", controller, value);
    DIAG_LOGI("ble", "Sending Control Change cc=%u value=%u", controller, value);
    return amp_transport_send_ble_midi(transport, message, sizeof(message));
}

static bool amp_transport_next_effect_state(amp_transport_t *transport, app_effect_id_t effect) {
    app_state_snapshot_t snapshot;
    app_state_get(transport->state, &snapshot);
    return effect < APP_EFFECT_COUNT ? !snapshot.effects[effect] : false;
}

static esp_err_t amp_transport_dispatch_preset(
    amp_transport_t *transport,
    app_preset_id_t preset,
    app_pc_offset_mode_t override_mode,
    bool use_override) {
    midi_config_snapshot_t midi_config;
    midi_config_get(transport->midi_config, &midi_config);
    if (use_override) {
        midi_config.pc_offset_mode = override_mode;
    }

    uint8_t program = 0;
    if (!midi_config_get_program_for_preset(&midi_config, preset, &program)) {
        ESP_LOGW(TAG, "Preset dispatch skipped because PC mapping is not calibrated");
        DIAG_LOGW("midi", "Preset dispatch skipped because PC offset is not calibrated");
        return ESP_ERR_INVALID_STATE;
    }

    return amp_transport_send_program_change(transport, program);
}

static esp_err_t amp_transport_dispatch_panel(amp_transport_t *transport) {
    midi_config_snapshot_t midi_config;
    midi_config_get(transport->midi_config, &midi_config);

    uint8_t program = 0;
    if (!midi_config_get_program_for_panel(&midi_config, &program)) {
        ESP_LOGW(TAG, "Panel dispatch skipped because PC mapping is not calibrated");
        DIAG_LOGW("midi", "Panel dispatch skipped because PC offset is not calibrated");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Dispatching PANEL Program Change program=%u", program);
    DIAG_LOGI("midi", "Dispatching PANEL as Program Change %u", program);
    return amp_transport_send_program_change(transport, program);
}

static esp_err_t amp_transport_dispatch_effect(amp_transport_t *transport, app_effect_id_t effect) {
    midi_config_snapshot_t midi_config;
    midi_config_get(transport->midi_config, &midi_config);

    uint8_t controller = 0;
    if (!midi_config_get_cc_for_effect(&midi_config, effect, &controller)) {
        ESP_LOGW(TAG, "Effect dispatch skipped because CC mapping is missing");
        DIAG_LOGW("midi", "Effect dispatch skipped because CC mapping is missing");
        return ESP_ERR_INVALID_STATE;
    }

    const bool enabled = amp_transport_next_effect_state(transport, effect);
    ESP_LOGI(TAG, "Dispatching %s toggle as CC%u -> %u", effect_name(effect), controller, enabled);
    DIAG_LOGI("midi", "Dispatching %s as CC%u -> %u", effect_name(effect), controller, enabled);
    return amp_transport_send_control_change(
        transport,
        controller,
        enabled ? MIDI_SWITCH_ON : MIDI_SWITCH_OFF);
}

static int amp_transport_on_subscribe_complete(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    struct ble_gatt_attr *attr,
    void *arg) {
    amp_transport_t *transport = arg;
    amp_transport_lock(transport);
    const bool connected = transport->conn_handle == conn_handle && error->status == 0;
    transport->subscription_active = connected;
    amp_transport_unlock(transport);

    if (!connected) {
        ESP_LOGE(TAG, "CCCD subscribe failed; status=%d", error->status);
        DIAG_LOGE("ble", "BLE notify subscribe failed status=%d", error->status);
        app_state_set_ble(transport->state, APP_BLE_ERROR);
        return 0;
    }

    ESP_LOGI(TAG, "BLE-MIDI notifications enabled on handle=%u", attr != NULL ? attr->handle : 0);
    DIAG_LOGI("ble", "BLE-MIDI notifications enabled");
    app_state_set_ble(transport->state, APP_BLE_CONNECTED);
    app_state_mark_synced(transport->state, false);
    return 0;
}

static int amp_transport_on_descriptor_discovered(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    uint16_t chr_val_handle,
    const struct ble_gatt_dsc *dsc,
    void *arg) {
    amp_transport_t *transport = arg;

    if (error->status == 0 && dsc != NULL) {
        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            amp_transport_lock(transport);
            transport->midi_cccd_handle = dsc->handle;
            amp_transport_unlock(transport);
            ESP_LOGI(TAG, "Found MIDI CCCD handle=%u for value_handle=%u", dsc->handle, chr_val_handle);
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "Descriptor discovery failed; status=%d", error->status);
        DIAG_LOGE("ble", "Descriptor discovery failed status=%d", error->status);
        app_state_set_ble(transport->state, APP_BLE_ERROR);
        return 0;
    }

    amp_transport_lock(transport);
    const uint16_t cccd_handle = transport->midi_cccd_handle;
    amp_transport_unlock(transport);

    if (cccd_handle == 0) {
        ESP_LOGE(TAG, "BLE-MIDI CCCD not found");
        DIAG_LOGE("ble", "BLE-MIDI notify descriptor not found");
        app_state_set_ble(transport->state, APP_BLE_ERROR);
        return 0;
    }

    uint8_t cccd_value[2] = {1, 0};
    const int rc = ble_gattc_write_flat(
        conn_handle,
        cccd_handle,
        cccd_value,
        sizeof(cccd_value),
        amp_transport_on_subscribe_complete,
        transport);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to enable BLE-MIDI notifications; rc=%d", rc);
        DIAG_LOGE("ble", "Failed to enable BLE notifications rc=%d", rc);
        app_state_set_ble(transport->state, APP_BLE_ERROR);
    }

    return 0;
}

static int amp_transport_on_characteristic_discovered(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_chr *chr,
    void *arg) {
    amp_transport_t *transport = arg;

    if (error->status == 0 && chr != NULL) {
        amp_transport_lock(transport);
        transport->midi_char_val_handle = chr->val_handle;
        transport->midi_char_properties = chr->properties;
        amp_transport_unlock(transport);
        ESP_LOGI(
            TAG,
            "Found BLE-MIDI characteristic value_handle=%u properties=0x%02X",
            chr->val_handle,
            chr->properties);
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "Characteristic discovery failed; status=%d", error->status);
        DIAG_LOGE("ble", "Characteristic discovery failed status=%d", error->status);
        app_state_set_ble(transport->state, APP_BLE_ERROR);
        return 0;
    }

    amp_transport_lock(transport);
    const uint16_t char_val_handle = transport->midi_char_val_handle;
    const uint16_t service_end_handle = transport->midi_service_end_handle;
    amp_transport_unlock(transport);

    if (char_val_handle == 0) {
        ESP_LOGE(TAG, "BLE-MIDI characteristic not found");
        DIAG_LOGE("ble", "BLE-MIDI characteristic not found");
        app_state_set_ble(transport->state, APP_BLE_ERROR);
        return 0;
    }

    const int rc = ble_gattc_disc_all_dscs(
        conn_handle,
        char_val_handle,
        service_end_handle,
        amp_transport_on_descriptor_discovered,
        transport);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start descriptor discovery; rc=%d", rc);
        DIAG_LOGE("ble", "Failed to start descriptor discovery rc=%d", rc);
        app_state_set_ble(transport->state, APP_BLE_ERROR);
    }

    return 0;
}

static int amp_transport_on_service_discovered(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_svc *service,
    void *arg) {
    amp_transport_t *transport = arg;

    if (error->status == 0 && service != NULL) {
        amp_transport_lock(transport);
        transport->midi_service_start_handle = service->start_handle;
        transport->midi_service_end_handle = service->end_handle;
        amp_transport_unlock(transport);
        ESP_LOGI(TAG, "Found BLE-MIDI service range=%u-%u", service->start_handle, service->end_handle);
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "Service discovery failed; status=%d", error->status);
        DIAG_LOGE("ble", "Service discovery failed status=%d", error->status);
        app_state_set_ble(transport->state, APP_BLE_ERROR);
        return 0;
    }

    amp_transport_lock(transport);
    const uint16_t start_handle = transport->midi_service_start_handle;
    const uint16_t end_handle = transport->midi_service_end_handle;
    amp_transport_unlock(transport);

    if (start_handle == 0 || end_handle == 0) {
        ESP_LOGE(TAG, "BLE-MIDI service not found on connected peer");
        DIAG_LOGE("ble", "BLE-MIDI service not found on peer");
        app_state_set_ble(transport->state, APP_BLE_ERROR);
        return 0;
    }

    const int rc = ble_gattc_disc_chrs_by_uuid(
        conn_handle,
        start_handle,
        end_handle,
        &s_midi_char_uuid.u,
        amp_transport_on_characteristic_discovered,
        transport);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start characteristic discovery; rc=%d", rc);
        DIAG_LOGE("ble", "Failed to start characteristic discovery rc=%d", rc);
        app_state_set_ble(transport->state, APP_BLE_ERROR);
    }

    return 0;
}

static void amp_transport_start_gatt_ready_flow(amp_transport_t *transport, uint16_t conn_handle) {
    amp_transport_lock(transport);
    const bool already_started = transport->gatt_ready;
    if (!already_started) {
        transport->gatt_ready = true;
    }
    amp_transport_unlock(transport);

    if (already_started) {
        return;
    }

    const int rc = ble_gattc_exchange_mtu(conn_handle, amp_transport_on_mtu_exchanged, transport);
    if (rc != 0) {
        ESP_LOGW(TAG, "MTU exchange start failed; rc=%d, continuing with service discovery", rc);
        DIAG_LOGW("ble", "MTU exchange start failed rc=%d; continuing", rc);
        amp_transport_on_mtu_exchanged(
            conn_handle,
            &(struct ble_gatt_error){ .status = 0, .att_handle = 0 },
            23,
            transport);
    }
}

static int amp_transport_on_mtu_exchanged(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    uint16_t mtu,
    void *arg) {
    amp_transport_t *transport = arg;
    ESP_LOGI(TAG, "MTU exchange status=%d mtu=%u", error->status, mtu);

    const int rc = ble_gattc_disc_svc_by_uuid(
        conn_handle,
        &s_midi_service_uuid.u,
        amp_transport_on_service_discovered,
        transport);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start BLE-MIDI service discovery; rc=%d", rc);
        DIAG_LOGE("ble", "Failed to start BLE-MIDI service discovery rc=%d", rc);
        app_state_set_ble(transport->state, APP_BLE_ERROR);
    }

    return 0;
}

static bool amp_transport_is_busy(amp_transport_t *transport) {
    return transport->scan_active || transport->connect_in_progress || transport->connected;
}

static void amp_transport_start_scan(amp_transport_t *transport) {
    struct ble_gap_disc_params params = {0};

    amp_transport_lock(transport);
    const bool can_scan = transport->host_ready && transport->connect_requested && !amp_transport_is_busy(transport);
    const uint8_t own_addr_type = transport->own_addr_type;
    if (can_scan) {
        transport->scan_active = true;
    }
    amp_transport_unlock(transport);

    if (!can_scan) {
        return;
    }

    params.filter_duplicates = 1;
    params.passive = 0;

    app_state_set_ble(transport->state, APP_BLE_SCANNING);
    ESP_LOGI(TAG, "Scanning for BLE MIDI peripheral '%s'", KATANA_MIDI_DEVICE_NAME);
    DIAG_LOGI("ble", "Scanning for '%s'", KATANA_MIDI_DEVICE_NAME);

    const int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, amp_transport_gap_event, transport);
    if (rc != 0) {
        amp_transport_lock(transport);
        transport->scan_active = false;
        amp_transport_unlock(transport);
        ESP_LOGE(TAG, "Failed to start BLE scan; rc=%d", rc);
        DIAG_LOGE("ble", "Failed to start BLE scan rc=%d", rc);
        app_state_set_ble(transport->state, APP_BLE_ERROR);
    }
}

static void amp_transport_restart_scan_if_needed(amp_transport_t *transport) {
    amp_transport_lock(transport);
    const bool should_scan = transport->connect_requested && !transport->connected && !transport->scan_active;
    amp_transport_unlock(transport);
    if (should_scan) {
        amp_transport_start_scan(transport);
    }
}

static int amp_transport_gap_event(struct ble_gap_event *event, void *arg) {
    amp_transport_t *transport = arg;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields;
            memset(&fields, 0, sizeof(fields));
            if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
                return 0;
            }

            if (!adv_name_matches(&fields)) {
                return 0;
            }

            char addr[18];
            format_ble_addr(&event->disc.addr, addr, sizeof(addr));
            ESP_LOGI(TAG, "Found target BLE MIDI peer '%s' at %s", KATANA_MIDI_DEVICE_NAME, addr);
            DIAG_LOGI("ble", "Found target peer at %s", addr);

            const int cancel_rc = ble_gap_disc_cancel();
            if (cancel_rc != 0 && cancel_rc != BLE_HS_EALREADY) {
                ESP_LOGE(TAG, "Failed to cancel scan before connect; rc=%d", cancel_rc);
                DIAG_LOGE("ble", "Failed to cancel scan before connect rc=%d", cancel_rc);
                return 0;
            }

            amp_transport_lock(transport);
            transport->scan_active = false;
            transport->connect_in_progress = true;
            amp_transport_unlock(transport);
            app_state_set_ble(transport->state, APP_BLE_CONNECTING);

            const int rc = ble_gap_connect(
                transport->own_addr_type,
                &event->disc.addr,
                30000,
                NULL,
                amp_transport_gap_event,
                transport);
            if (rc != 0) {
                amp_transport_lock(transport);
                transport->connect_in_progress = false;
                amp_transport_unlock(transport);
                ESP_LOGE(TAG, "Failed to initiate BLE connection; rc=%d", rc);
                DIAG_LOGE("ble", "Failed to initiate BLE connection rc=%d", rc);
                amp_transport_restart_scan_if_needed(transport);
            }
            return 0;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            amp_transport_lock(transport);
            transport->scan_active = false;
            amp_transport_unlock(transport);
            return 0;

        case BLE_GAP_EVENT_CONNECT:
            amp_transport_lock(transport);
            transport->connect_in_progress = false;
            if (event->connect.status == 0) {
                transport->connected = true;
                transport->conn_handle = event->connect.conn_handle;
                amp_transport_reset_protocol_state(transport);
                transport->security_in_progress = false;
                transport->link_encrypted = false;
                transport->gatt_ready = false;
                transport->subscription_active = false;
            }
            amp_transport_unlock(transport);

            if (event->connect.status != 0) {
                ESP_LOGE(TAG, "BLE connection failed; status=%d", event->connect.status);
                DIAG_LOGE("ble", "BLE connection failed status=%d", event->connect.status);
                app_state_set_ble(transport->state, APP_BLE_ERROR);
                amp_transport_restart_scan_if_needed(transport);
                return 0;
            }

            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                char addr[18];
                format_ble_addr(&desc.peer_id_addr, addr, sizeof(addr));
                ESP_LOGI(TAG, "BLE link established with %s", addr);
                DIAG_LOGI("ble", "BLE link established with %s", addr);
            }

            app_state_set_ble(transport->state, APP_BLE_CONNECTING);
            const int rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to initiate BLE security; rc=%d", rc);
                DIAG_LOGE("ble", "Failed to initiate BLE security rc=%d", rc);
                app_state_set_ble(transport->state, APP_BLE_ERROR);
                return 0;
            }
            amp_transport_lock(transport);
            transport->security_in_progress = true;
            amp_transport_unlock(transport);
            ESP_LOGI(TAG, "BLE security initiation started");
            DIAG_LOGI("ble", "BLE security negotiation started");
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "BLE disconnected; reason=%d", event->disconnect.reason);
            DIAG_LOGW("ble", "BLE disconnected reason=%d", event->disconnect.reason);
            amp_transport_lock(transport);
            transport->connected = false;
            transport->conn_handle = 0;
            amp_transport_reset_protocol_state(transport);
            transport->security_in_progress = false;
            transport->link_encrypted = false;
            transport->gatt_ready = false;
            transport->subscription_active = false;
            amp_transport_unlock(transport);
            app_state_set_ble(transport->state, APP_BLE_DISCONNECTED);
            app_state_mark_synced(transport->state, false);
            amp_transport_restart_scan_if_needed(transport);
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "BLE encryption change status=%d", event->enc_change.status);
            amp_transport_lock(transport);
            const bool enc_conn_matches = transport->conn_handle == event->enc_change.conn_handle;
            if (enc_conn_matches) {
                transport->security_in_progress = false;
                transport->link_encrypted = event->enc_change.status == 0;
            }
            amp_transport_unlock(transport);

            if (!enc_conn_matches) {
                return 0;
            }

            if (event->enc_change.status != 0) {
                ESP_LOGE(TAG, "BLE encryption failed; status=%d", event->enc_change.status);
                DIAG_LOGE("ble", "BLE encryption failed status=%d", event->enc_change.status);
                app_state_set_ble(transport->state, APP_BLE_ERROR);
                return 0;
            }

            ESP_LOGI(TAG, "BLE link encrypted");
            DIAG_LOGI("ble", "BLE link encrypted");
            amp_transport_start_gatt_ready_flow(transport, event->enc_change.conn_handle);
            return 0;

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            struct ble_gap_conn_desc desc;
            int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
            if (rc == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
                ESP_LOGW(TAG, "Deleted previous bond and retrying BLE pairing");
                DIAG_LOGW("ble", "Deleted stale bond and retrying pairing");
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        case BLE_GAP_EVENT_PARING_COMPLETE:
            ESP_LOGI(
                TAG,
                "BLE pairing complete conn_handle=%u status=%d",
                event->pairing_complete.conn_handle,
                event->pairing_complete.status);
            DIAG_LOGI("ble", "BLE pairing complete status=%d", event->pairing_complete.status);
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX: {
            amp_transport_lock(transport);
            const bool is_midi_attr =
                event->notify_rx.conn_handle == transport->conn_handle
                && event->notify_rx.attr_handle == transport->midi_char_val_handle;
            amp_transport_unlock(transport);

            if (!is_midi_attr) {
                return 0;
            }

            const uint16_t pkt_len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t buffer[256];
            const uint16_t copy_len = pkt_len > sizeof(buffer) ? sizeof(buffer) : pkt_len;
            if (os_mbuf_copydata(event->notify_rx.om, 0, copy_len, buffer) == 0) {
                amp_transport_parse_ble_midi_packet(transport, buffer, copy_len);
            }
            return 0;
        }

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "BLE MTU updated: %u", event->mtu.value);
            DIAG_LOGI("ble", "BLE MTU updated to %u", event->mtu.value);
            return 0;

        default:
            return 0;
    }
}

static void amp_transport_on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE reset; reason=%d", reason);
    DIAG_LOGW("ble", "NimBLE reset reason=%d", reason);
    if (s_transport == NULL) {
        return;
    }

    amp_transport_lock(s_transport);
    s_transport->host_ready = false;
    s_transport->scan_active = false;
    s_transport->connect_in_progress = false;
    s_transport->connected = false;
    s_transport->security_in_progress = false;
    s_transport->link_encrypted = false;
    s_transport->gatt_ready = false;
    s_transport->conn_handle = 0;
    s_transport->subscription_active = false;
    amp_transport_reset_protocol_state(s_transport);
    amp_transport_unlock(s_transport);

    app_state_set_ble(s_transport->state, APP_BLE_DISCONNECTED);
    app_state_mark_synced(s_transport->state, false);
}

static void amp_transport_on_sync(void) {
    int rc;

    if (s_transport == NULL) {
        return;
    }

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure BLE address; rc=%d", rc);
        DIAG_LOGE("ble", "Failed to ensure BLE address rc=%d", rc);
        app_state_set_ble(s_transport->state, APP_BLE_ERROR);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_transport->own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer own BLE address type; rc=%d", rc);
        DIAG_LOGE("ble", "Failed to infer BLE address type rc=%d", rc);
        app_state_set_ble(s_transport->state, APP_BLE_ERROR);
        return;
    }

    amp_transport_lock(s_transport);
    s_transport->host_ready = true;
    amp_transport_unlock(s_transport);

    ESP_LOGI(TAG, "NimBLE host synced; addr_type=%u", s_transport->own_addr_type);
    DIAG_LOGI("ble", "NimBLE host synced");
    amp_transport_start_scan(s_transport);
}

esp_err_t amp_transport_init(amp_transport_t *transport, const amp_transport_config_t *config) {
    memset(transport, 0, sizeof(*transport));
    transport->state = config->state;
    transport->midi_config = config->midi_config;
    transport->mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(transport->mutex != NULL, ESP_ERR_NO_MEM, TAG, "create transport mutex");

    const esp_err_t err = nimble_port_init();
    ESP_RETURN_ON_ERROR(err, TAG, "init NimBLE");

    ble_hs_cfg.reset_cb = amp_transport_on_reset;
    ble_hs_cfg.sync_cb = amp_transport_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    int rc = ble_svc_gap_device_name_set(APP_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set GAP name; rc=%d", rc);
        return ESP_FAIL;
    }
#endif

    ble_store_config_init();
    s_transport = transport;
    nimble_port_freertos_init(amp_transport_host_task);

    ESP_LOGI(TAG, "Amp transport initialized in Gen 3 MIDI-only mode");
    DIAG_LOGI("ble", "Initialized Gen 3 MIDI-only BLE transport");
    return ESP_OK;
}

esp_err_t amp_transport_poll_read(amp_transport_t *transport) {
    (void) transport;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t amp_transport_connect(amp_transport_t *transport) {
    amp_transport_lock(transport);
    transport->connect_requested = true;
    amp_transport_unlock(transport);

    app_state_mark_synced(transport->state, false);
    amp_transport_start_scan(transport);
    return ESP_OK;
}

esp_err_t amp_transport_disconnect(amp_transport_t *transport) {
    uint16_t conn_handle = 0;
    bool scan_active = false;

    amp_transport_lock(transport);
    transport->connect_requested = false;
    const bool connected = transport->connected;
    conn_handle = transport->conn_handle;
    scan_active = transport->scan_active;
    amp_transport_unlock(transport);

    if (scan_active) {
        ble_gap_disc_cancel();
    }
    if (connected) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        app_state_set_ble(transport->state, APP_BLE_DISCONNECTED);
        app_state_mark_synced(transport->state, false);
    }

    ESP_LOGI(TAG, "BLE transport disconnect requested");
    DIAG_LOGW("ble", "BLE transport disconnect requested");
    return ESP_OK;
}

bool amp_transport_should_apply_optimistic_action(const amp_transport_t *transport, const app_action_t *action) {
    (void) transport;

    switch (action->type) {
        case APP_ACTION_PRESET_SELECT:
        case APP_ACTION_PANEL_SELECT:
        case APP_ACTION_MODE_SET:
        case APP_ACTION_EFFECT_TOGGLE:
        case APP_ACTION_BLE_RECONNECT:
        case APP_ACTION_WIFI_RESET:
            return true;
        case APP_ACTION_RESYNC:
            return false;
    }

    return false;
}

esp_err_t amp_transport_dispatch_action(amp_transport_t *transport, const app_action_t *action) {
    switch (action->type) {
        case APP_ACTION_BLE_RECONNECT:
            ESP_LOGI(TAG, "Manual BLE reconnect requested");
            DIAG_LOGW("ble", "Manual BLE reconnect requested");
            return amp_transport_connect(transport);
        case APP_ACTION_WIFI_RESET:
            return ESP_OK;
        case APP_ACTION_MODE_SET:
            ESP_LOGI(
                TAG,
                "Local footswitch mode changed to %s",
                action->value.mode == APP_FOOTSWITCH_MODE_PRESET ? "preset" : "effect");
            DIAG_LOGI(
                "action",
                "Changed local mode to %s",
                action->value.mode == APP_FOOTSWITCH_MODE_PRESET ? "preset" : "effect");
            return ESP_OK;
        case APP_ACTION_RESYNC:
            return amp_transport_resync(transport);
        case APP_ACTION_PRESET_SELECT:
            return amp_transport_dispatch_preset(transport, action->value.preset, APP_PC_OFFSET_UNKNOWN, false);
        case APP_ACTION_PANEL_SELECT:
            return amp_transport_dispatch_panel(transport);
        case APP_ACTION_EFFECT_TOGGLE:
            return amp_transport_dispatch_effect(transport, action->value.effect);
    }

    return ESP_OK;
}

esp_err_t amp_transport_resync(amp_transport_t *transport) {
    (void) transport;
    ESP_LOGI(TAG, "Resync requested, but Gen 3 state feedback is intentionally disabled in default runtime");
    DIAG_LOGI("midi", "Resync requested; amp feedback remains intentionally disabled");
    app_state_mark_synced(transport->state, false);
    return ESP_OK;
}

esp_err_t amp_transport_run_pc_offset_test(
    amp_transport_t *transport,
    app_preset_id_t preset,
    app_pc_offset_mode_t pc_offset_mode) {
    if (pc_offset_mode == APP_PC_OFFSET_UNKNOWN) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(
        TAG,
        "Running PC offset calibration test for preset=%d mode=%d",
        preset,
        pc_offset_mode);
    DIAG_LOGI("midi", "Running PC offset test for preset=%d mode=%d", preset, pc_offset_mode);
    return amp_transport_dispatch_preset(transport, preset, pc_offset_mode, true);
}

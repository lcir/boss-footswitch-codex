#include "web_server.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "web_ui.h"
#include "wifi_manager.h"

static const char *TAG = "web_server";

static web_server_config_t s_config;
static httpd_handle_t s_server;

typedef struct {
    int fd;
    char *payload;
} ws_send_job_t;

static const char *preset_to_string(app_preset_id_t preset) {
    static const char *labels[] = { "A1", "A2", "B1", "B2" };
    return labels[preset];
}

static const char *effect_to_string(app_effect_id_t effect) {
    static const char *labels[] = { "booster", "mod", "fx", "delay", "reverb" };
    return labels[effect];
}

static const char *mode_to_string(app_footswitch_mode_t mode) {
    return mode == APP_FOOTSWITCH_MODE_PRESET ? "preset" : "effect";
}

static const char *ble_state_to_string(app_ble_state_t state) {
    static const char *labels[] = { "disconnected", "scanning", "connecting", "connected", "error" };
    return labels[state];
}

static const char *wifi_state_to_string(app_wifi_state_t state) {
    static const char *labels[] = { "unprovisioned", "ap", "connecting", "connected", "error" };
    return labels[state];
}

static const char *pc_offset_mode_to_string(app_pc_offset_mode_t mode) {
    switch (mode) {
        case APP_PC_OFFSET_SUBTRACT_ONE: return "subtract-one";
        case APP_PC_OFFSET_DIRECT: return "direct";
        case APP_PC_OFFSET_UNKNOWN: return "unknown";
    }

    return "unknown";
}

static const char *amp_state_confidence_to_string(app_amp_state_confidence_t confidence) {
    switch (confidence) {
        case APP_AMP_STATE_CONFIDENCE_LOW: return "low";
        case APP_AMP_STATE_CONFIDENCE_MEDIUM: return "medium";
        case APP_AMP_STATE_CONFIDENCE_HIGH: return "high";
        case APP_AMP_STATE_CONFIDENCE_NONE: return "none";
    }

    return "none";
}

static bool parse_pc_offset_mode_string(const char *value, app_pc_offset_mode_t *out_mode) {
    if (strcmp(value, "subtract-one") == 0) {
        *out_mode = APP_PC_OFFSET_SUBTRACT_ONE;
        return true;
    }
    if (strcmp(value, "direct") == 0) {
        *out_mode = APP_PC_OFFSET_DIRECT;
        return true;
    }
    if (strcmp(value, "unknown") == 0) {
        *out_mode = APP_PC_OFFSET_UNKNOWN;
        return true;
    }
    return false;
}

static bool parse_preset_string(const char *value, app_preset_id_t *out_preset) {
    if (strcmp(value, "A1") == 0) {
        *out_preset = APP_PRESET_A1;
        return true;
    }
    if (strcmp(value, "A2") == 0) {
        *out_preset = APP_PRESET_A2;
        return true;
    }
    if (strcmp(value, "B1") == 0) {
        *out_preset = APP_PRESET_B1;
        return true;
    }
    if (strcmp(value, "B2") == 0) {
        *out_preset = APP_PRESET_B2;
        return true;
    }
    return false;
}

static cJSON *snapshot_to_json(const app_state_snapshot_t *snapshot) {
    cJSON *root = cJSON_CreateObject();
    cJSON *effects = cJSON_AddObjectToObject(root, "effects");

    cJSON_AddStringToObject(root, "footswitchMode", mode_to_string(snapshot->footswitch_mode));
    cJSON_AddStringToObject(root, "bleState", ble_state_to_string(snapshot->ble_state));
    cJSON_AddStringToObject(root, "wifiState", wifi_state_to_string(snapshot->wifi_state));
    cJSON_AddStringToObject(root, "activePreset", preset_to_string(snapshot->active_preset));
    cJSON_AddStringToObject(root, "pcOffsetMode", pc_offset_mode_to_string(snapshot->pc_offset_mode));
    cJSON_AddStringToObject(root, "ampStateConfidence", amp_state_confidence_to_string(snapshot->amp_state_confidence));
    cJSON_AddBoolToObject(root, "soloEnabled", snapshot->solo_enabled);
    cJSON_AddBoolToObject(root, "synced", snapshot->synced);
    cJSON_AddBoolToObject(root, "midiConfigured", snapshot->midi_configured);
    cJSON_AddBoolToObject(root, "soloConfigured", snapshot->solo_configured);
    cJSON_AddBoolToObject(root, "wifiProvisioned", snapshot->wifi_provisioned);
    cJSON_AddStringToObject(root, "wifiSsid", snapshot->wifi.ssid);
    cJSON_AddStringToObject(root, "wifiIp", snapshot->wifi.ip);
    cJSON_AddStringToObject(root, "lastError", snapshot->last_error);

    for (int i = 0; i < APP_EFFECT_COUNT; ++i) {
        cJSON_AddBoolToObject(effects, effect_to_string((app_effect_id_t) i), snapshot->effects[i]);
    }

    return root;
}

static cJSON *midi_config_to_json(const midi_config_snapshot_t *midi_config) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "pcPanel", midi_config->pc_panel);
    cJSON_AddNumberToObject(root, "pcA1", midi_config->pc_a1);
    cJSON_AddNumberToObject(root, "pcA2", midi_config->pc_a2);
    cJSON_AddNumberToObject(root, "pcB1", midi_config->pc_b1);
    cJSON_AddNumberToObject(root, "pcB2", midi_config->pc_b2);
    cJSON_AddNumberToObject(root, "ccBooster", midi_config->cc_booster);
    cJSON_AddNumberToObject(root, "ccMod", midi_config->cc_mod);
    cJSON_AddNumberToObject(root, "ccFx", midi_config->cc_fx);
    cJSON_AddNumberToObject(root, "ccDelay", midi_config->cc_delay);
    cJSON_AddNumberToObject(root, "ccReverb", midi_config->cc_reverb);
    cJSON_AddNumberToObject(root, "ccSendReturn", midi_config->cc_send_return);
    cJSON_AddBoolToObject(root, "soloConfigured", midi_config->solo_configured);
    cJSON_AddNumberToObject(root, "ccSolo", midi_config->cc_solo);
    cJSON_AddStringToObject(root, "pcOffsetMode", pc_offset_mode_to_string(midi_config->pc_offset_mode));
    cJSON_AddBoolToObject(root, "midiConfigured", midi_config_is_ready(midi_config));
    return root;
}

static esp_err_t send_json_object(httpd_req_t *req, cJSON *root) {
    char *response = cJSON_PrintUnformatted(root);
    if (response == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON encode failed");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, response);
    cJSON_Delete(root);
    cJSON_free(response);
    return err;
}

static esp_err_t send_state_response(httpd_req_t *req, const app_state_snapshot_t *snapshot) {
    return send_json_object(req, snapshot_to_json(snapshot));
}

static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, web_ui_index_html());
}

static esp_err_t redirect_to_root_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t state_get_handler(httpd_req_t *req) {
    app_state_snapshot_t snapshot;
    app_state_get(s_config.state, &snapshot);
    return send_state_response(req, &snapshot);
}

static esp_err_t midi_config_get_handler(httpd_req_t *req) {
    midi_config_snapshot_t midi_config;
    midi_config_get(s_config.midi_config, &midi_config);
    return send_json_object(req, midi_config_to_json(&midi_config));
}

static bool parse_action_json(cJSON *root, app_action_t *action) {
    const cJSON *action_name = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(action_name) || action_name->valuestring == NULL) {
        return false;
    }

    if (strcmp(action_name->valuestring, "preset.select") == 0) {
        const cJSON *preset = cJSON_GetObjectItemCaseSensitive(root, "preset");
        action->type = APP_ACTION_PRESET_SELECT;
        return cJSON_IsString(preset) && parse_preset_string(preset->valuestring, &action->value.preset);
    }

    if (strcmp(action_name->valuestring, "solo.toggle") == 0) {
        action->type = APP_ACTION_SOLO_TOGGLE;
        return true;
    }

    if (strcmp(action_name->valuestring, "mode.set") == 0) {
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        action->type = APP_ACTION_MODE_SET;
        action->value.mode = (cJSON_IsString(mode) && strcmp(mode->valuestring, "effect") == 0)
            ? APP_FOOTSWITCH_MODE_EFFECT
            : APP_FOOTSWITCH_MODE_PRESET;
        return true;
    }

    if (strcmp(action_name->valuestring, "effect.toggle") == 0) {
        const cJSON *effect = cJSON_GetObjectItemCaseSensitive(root, "effect");
        action->type = APP_ACTION_EFFECT_TOGGLE;
        if (!cJSON_IsString(effect)) {
            return false;
        }
        if (strcmp(effect->valuestring, "booster") == 0) action->value.effect = APP_EFFECT_BOOSTER;
        else if (strcmp(effect->valuestring, "mod") == 0) action->value.effect = APP_EFFECT_MOD;
        else if (strcmp(effect->valuestring, "fx") == 0) action->value.effect = APP_EFFECT_FX;
        else if (strcmp(effect->valuestring, "delay") == 0) action->value.effect = APP_EFFECT_DELAY;
        else if (strcmp(effect->valuestring, "reverb") == 0) action->value.effect = APP_EFFECT_REVERB;
        else return false;
        return true;
    }

    if (strcmp(action_name->valuestring, "ble.reconnect") == 0) {
        action->type = APP_ACTION_BLE_RECONNECT;
        return true;
    }

    if (strcmp(action_name->valuestring, "wifi.reset") == 0) {
        action->type = APP_ACTION_WIFI_RESET;
        return true;
    }

    if (strcmp(action_name->valuestring, "resync") == 0) {
        action->type = APP_ACTION_RESYNC;
        return true;
    }

    return false;
}

static esp_err_t receive_body(httpd_req_t *req, char *buffer, size_t buffer_size) {
    const int received = httpd_req_recv(req, buffer, buffer_size - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
    }
    buffer[received] = '\0';
    return ESP_OK;
}

static esp_err_t action_post_handler(httpd_req_t *req) {
    char buffer[384];
    ESP_RETURN_ON_ERROR(receive_body(req, buffer, sizeof(buffer)), TAG, "receive action body");

    cJSON *root = cJSON_Parse(buffer);
    app_action_t action = {0};
    if (root == NULL || !parse_action_json(root, &action)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid action payload");
    }

    s_config.on_action(&action, s_config.user_ctx);
    cJSON_Delete(root);
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "accepted");
}

static esp_err_t provision_post_handler(httpd_req_t *req) {
    char buffer[256];
    ESP_RETURN_ON_ERROR(receive_body(req, buffer, sizeof(buffer)), TAG, "receive provision body");

    cJSON *root = cJSON_Parse(buffer);
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing credentials");
    }

    esp_err_t err = wifi_manager_save_credentials(ssid->valuestring, password->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Provisioning failed");
    }

    return httpd_resp_sendstr(req, "saved");
}

static bool read_u8_field(cJSON *root, const char *name, uint8_t *target) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > 127) {
        return false;
    }
    *target = (uint8_t) item->valueint;
    return true;
}

static esp_err_t midi_config_post_handler(httpd_req_t *req) {
    char buffer[768];
    ESP_RETURN_ON_ERROR(receive_body(req, buffer, sizeof(buffer)), TAG, "receive midi config body");

    cJSON *root = cJSON_Parse(buffer);
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    midi_config_snapshot_t midi_config;
    midi_config_get(s_config.midi_config, &midi_config);

    bool ok =
        read_u8_field(root, "pcPanel", &midi_config.pc_panel)
        && read_u8_field(root, "pcA1", &midi_config.pc_a1)
        && read_u8_field(root, "pcA2", &midi_config.pc_a2)
        && read_u8_field(root, "pcB1", &midi_config.pc_b1)
        && read_u8_field(root, "pcB2", &midi_config.pc_b2)
        && read_u8_field(root, "ccBooster", &midi_config.cc_booster)
        && read_u8_field(root, "ccMod", &midi_config.cc_mod)
        && read_u8_field(root, "ccFx", &midi_config.cc_fx)
        && read_u8_field(root, "ccDelay", &midi_config.cc_delay)
        && read_u8_field(root, "ccReverb", &midi_config.cc_reverb)
        && read_u8_field(root, "ccSendReturn", &midi_config.cc_send_return)
        && read_u8_field(root, "ccSolo", &midi_config.cc_solo);

    cJSON *solo_configured = cJSON_GetObjectItemCaseSensitive(root, "soloConfigured");
    if (solo_configured != NULL) {
        if (!cJSON_IsBool(solo_configured)) {
            ok = false;
        } else {
            midi_config.solo_configured = cJSON_IsTrue(solo_configured);
        }
    }

    cJSON *pc_offset_mode = cJSON_GetObjectItemCaseSensitive(root, "pcOffsetMode");
    if (pc_offset_mode != NULL) {
        if (!cJSON_IsString(pc_offset_mode)
            || !parse_pc_offset_mode_string(pc_offset_mode->valuestring, &midi_config.pc_offset_mode)) {
            ok = false;
        }
    }

    if (!ok) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid MIDI config payload");
    }

    esp_err_t err = midi_config_save(s_config.midi_config, &midi_config);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save MIDI config");
    }

    app_state_set_midi_status(
        s_config.state,
        midi_config_is_ready(&midi_config),
        midi_config.solo_configured,
        midi_config.pc_offset_mode);

    return send_json_object(req, midi_config_to_json(&midi_config));
}

static esp_err_t midi_calibrate_post_handler(httpd_req_t *req) {
    char buffer[256];
    ESP_RETURN_ON_ERROR(receive_body(req, buffer, sizeof(buffer)), TAG, "receive calibration body");

    cJSON *root = cJSON_Parse(buffer);
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *candidate = cJSON_GetObjectItemCaseSensitive(root, "candidate");
    const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    const cJSON *preset = cJSON_GetObjectItemCaseSensitive(root, "preset");
    if (!cJSON_IsString(candidate)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing candidate");
    }

    app_pc_offset_mode_t candidate_mode;
    if (!parse_pc_offset_mode_string(candidate->valuestring, &candidate_mode)
        || candidate_mode == APP_PC_OFFSET_UNKNOWN) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid candidate");
    }

    app_preset_id_t target_preset = APP_PRESET_A1;
    if (cJSON_IsString(preset) && !parse_preset_string(preset->valuestring, &target_preset)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid preset");
    }

    if (cJSON_IsBool(confirm) && cJSON_IsTrue(confirm)) {
        midi_config_snapshot_t midi_config;
        midi_config_get(s_config.midi_config, &midi_config);
        midi_config.pc_offset_mode = candidate_mode;
        esp_err_t err = midi_config_save(s_config.midi_config, &midi_config);
        cJSON_Delete(root);
        if (err != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to store calibration");
        }

        app_state_set_midi_status(
            s_config.state,
            midi_config_is_ready(&midi_config),
            midi_config.solo_configured,
            midi_config.pc_offset_mode);
        return send_json_object(req, midi_config_to_json(&midi_config));
    }

    cJSON_Delete(root);
    const esp_err_t err = amp_transport_run_pc_offset_test(s_config.transport, target_preset, candidate_mode);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "BLE-MIDI transport not ready");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Calibration test failed");
    }

    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "test-sent");
}

static esp_err_t ws_get_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket client connected fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    return ESP_OK;
}

static void ws_async_send(void *arg) {
    ws_send_job_t *job = arg;
    httpd_ws_frame_t frame = {
        .payload = (uint8_t *) job->payload,
        .len = strlen(job->payload),
        .type = HTTPD_WS_TYPE_TEXT,
    };
    httpd_ws_send_frame_async(s_server, job->fd, &frame);
    free(job->payload);
    free(job);
}

esp_err_t web_server_init(const web_server_config_t *config) {
    memset(&s_config, 0, sizeof(s_config));
    s_config = *config;
    return ESP_OK;
}

esp_err_t web_server_start(void) {
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = APP_HTTP_MAX_CLIENTS;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "start server");

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    httpd_uri_t state_uri = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = state_get_handler,
    };
    httpd_uri_t action_uri = {
        .uri = "/api/action",
        .method = HTTP_POST,
        .handler = action_post_handler,
    };
    httpd_uri_t provision_uri = {
        .uri = "/api/provision",
        .method = HTTP_POST,
        .handler = provision_post_handler,
    };
    httpd_uri_t midi_config_get_uri = {
        .uri = "/api/midi-config",
        .method = HTTP_GET,
        .handler = midi_config_get_handler,
    };
    httpd_uri_t midi_config_post_uri = {
        .uri = "/api/midi-config",
        .method = HTTP_POST,
        .handler = midi_config_post_handler,
    };
    httpd_uri_t midi_calibrate_uri = {
        .uri = "/api/midi-calibrate",
        .method = HTTP_POST,
        .handler = midi_calibrate_post_handler,
    };
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_get_handler,
        .is_websocket = true,
    };
    httpd_uri_t captive_android = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = redirect_to_root_handler,
    };
    httpd_uri_t captive_apple = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = redirect_to_root_handler,
    };
    httpd_uri_t captive_windows = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = redirect_to_root_handler,
    };

    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &state_uri);
    httpd_register_uri_handler(s_server, &action_uri);
    httpd_register_uri_handler(s_server, &provision_uri);
    httpd_register_uri_handler(s_server, &midi_config_get_uri);
    httpd_register_uri_handler(s_server, &midi_config_post_uri);
    httpd_register_uri_handler(s_server, &midi_calibrate_uri);
    httpd_register_uri_handler(s_server, &ws_uri);
    httpd_register_uri_handler(s_server, &captive_android);
    httpd_register_uri_handler(s_server, &captive_apple);
    httpd_register_uri_handler(s_server, &captive_windows);
    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

esp_err_t web_server_broadcast_state(const app_state_snapshot_t *snapshot) {
    if (s_server == NULL) {
        return ESP_OK;
    }

    cJSON *root = snapshot_to_json(snapshot);
    char *payload = cJSON_PrintUnformatted(root);
    if (payload == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_Delete(root);

    size_t clients = 0;
    int client_fds[APP_HTTP_MAX_CLIENTS] = {0};
    if (httpd_get_client_list(s_server, &clients, client_fds) != ESP_OK) {
        free(payload);
        return ESP_FAIL;
    }

    for (size_t i = 0; i < clients; ++i) {
        if (httpd_ws_get_fd_info(s_server, client_fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
            continue;
        }

        ws_send_job_t *job = calloc(1, sizeof(*job));
        if (job == NULL) {
            continue;
        }
        job->fd = client_fds[i];
        job->payload = strdup(payload);
        if (job->payload == NULL) {
            free(job);
            continue;
        }

        if (httpd_queue_work(s_server, ws_async_send, job) != ESP_OK) {
            free(job->payload);
            free(job);
        }
    }

    free(payload);
    return ESP_OK;
}

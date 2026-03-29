#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "amp_transport.h"
#include "app_config.h"
#include "app_state.h"
#include "buttons.h"
#include "leds.h"
#include "midi_config.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "app_main";

typedef struct {
    app_state_store_t state;
    midi_config_store_t midi_config;
    amp_transport_t amp;
    QueueHandle_t action_queue;
    uint32_t rendered_state_version;
} app_context_t;

static app_context_t s_app;

static void queue_action(const app_action_t *action, void *user_ctx) {
    app_context_t *app = user_ctx;
    xQueueSend(app->action_queue, action, 0);
}

static void on_wifi_ready(void *user_ctx) {
    app_context_t *app = user_ctx;
    web_server_start();
    amp_transport_connect(&app->amp);
}

static void render_outputs_if_needed(app_context_t *app) {
    app_state_snapshot_t snapshot;
    app_state_get(&app->state, &snapshot);

    if (snapshot.state_version == app->rendered_state_version) {
        return;
    }

    leds_render_state(&snapshot);
    web_server_broadcast_state(&snapshot);
    app->rendered_state_version = snapshot.state_version;
}

static void process_action(app_context_t *app, const app_action_t *action) {
    ESP_LOGI(TAG, "Processing action %d", action->type);
    const esp_err_t dispatch_err = amp_transport_dispatch_action(&app->amp, action);
    if (dispatch_err == ESP_OK && amp_transport_should_apply_optimistic_action(&app->amp, action)) {
        app_state_apply_optimistic_action(&app->state, action);
    }

    if (action->type == APP_ACTION_WIFI_RESET) {
        wifi_manager_reset_credentials();
        return;
    }
}

static void controller_task(void *arg) {
    app_context_t *app = arg;
    app_action_t action;

    while (true) {
        if (xQueueReceive(app->action_queue, &action, pdMS_TO_TICKS(50)) == pdTRUE) {
            process_action(app, &action);
        }
        render_outputs_if_needed(app);
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    memset(&s_app, 0, sizeof(s_app));
    app_state_init(&s_app.state);
    s_app.action_queue = xQueueCreate(APP_STATE_BROADCAST_QUEUE_LEN, sizeof(app_action_t));

    ESP_ERROR_CHECK(midi_config_init(&s_app.midi_config));
    midi_config_snapshot_t midi_snapshot;
    midi_config_get(&s_app.midi_config, &midi_snapshot);
    app_state_set_midi_status(
        &s_app.state,
        midi_config_is_ready(&midi_snapshot),
        midi_snapshot.solo_configured,
        midi_snapshot.pc_offset_mode);

    ESP_ERROR_CHECK(leds_init());
    ESP_ERROR_CHECK(amp_transport_init(&s_app.amp, &(amp_transport_config_t){
        .state = &s_app.state,
        .midi_config = &s_app.midi_config,
    }));
    ESP_ERROR_CHECK(web_server_init(&(web_server_config_t){
        .state = &s_app.state,
        .midi_config = &s_app.midi_config,
        .transport = &s_app.amp,
        .on_action = queue_action,
        .user_ctx = &s_app,
    }));
    ESP_ERROR_CHECK(wifi_manager_init(&(wifi_manager_config_t){
        .state = &s_app.state,
        .on_wifi_ready = on_wifi_ready,
        .user_ctx = &s_app,
    }));
    ESP_ERROR_CHECK(buttons_init(&(buttons_config_t){
        .state = &s_app.state,
        .callback = queue_action,
        .user_ctx = &s_app,
    }));

    xTaskCreate(controller_task, "controller", 6144, &s_app, 5, NULL);

    if (wifi_manager_is_provisioned()) {
        app_state_set_runtime(&s_app.state, APP_RUNTIME_BLE_CONNECTING);
    } else {
        app_state_set_runtime(&s_app.state, APP_RUNTIME_WIFI_PROVISION);
    }

    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(wifi_manager_start());
    render_outputs_if_needed(&s_app);
}

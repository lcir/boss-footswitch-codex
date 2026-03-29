#include "wifi_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "diag_log.h"

static const char *TAG = "wifi_manager";
static const char *NVS_NS = "wifi";
static const char *NVS_KEY_SSID = "ssid";
static const char *NVS_KEY_PASS = "pass";

static wifi_manager_config_t s_config;
static bool s_provisioned;
static char s_ssid[33];
static char s_pass[65];

static esp_err_t load_credentials(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = sizeof(s_ssid);
    size_t pass_len = sizeof(s_pass);
    err = nvs_get_str(handle, NVS_KEY_SSID, s_ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, NVS_KEY_PASS, s_pass, &pass_len);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t start_softap(void) {
    wifi_config_t ap_config = {
        .ap = {
            .ssid = APP_SOFTAP_SSID,
            .password = APP_SOFTAP_PASS,
            .ssid_len = 0,
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    app_state_set_wifi_mode(s_config.state, APP_WIFI_AP_MODE);
    app_state_set_provisioned(s_config.state, false, true);
    memset(&s_ssid, 0, sizeof(s_ssid));
    ESP_LOGI(TAG, "Started provisioning SoftAP '%s'", APP_SOFTAP_SSID);
    DIAG_LOGW("wifi", "Started provisioning AP '%s'", APP_SOFTAP_SSID);
    return ESP_OK;
}

static esp_err_t start_station(void) {
    wifi_config_t sta_config = {0};

    memcpy(sta_config.sta.ssid, s_ssid, strlen(s_ssid));
    memcpy(sta_config.sta.password, s_pass, strlen(s_pass));
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    app_state_set_wifi_mode(s_config.state, APP_WIFI_CONNECTING);
    app_state_set_provisioned(s_config.state, true, false);
    ESP_LOGI(TAG, "Connecting to WiFi SSID '%s'", s_ssid);
    DIAG_LOGI("wifi", "Connecting to SSID '%s'", s_ssid);
    return ESP_OK;
}

static void handle_got_ip(void) {
    esp_netif_ip_info_t ip = {0};
    app_wifi_snapshot_t snapshot = {
        .connected = true,
    };

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(snapshot.ip, sizeof(snapshot.ip), IPSTR, IP2STR(&ip.ip));
    }
    strlcpy(snapshot.ssid, s_ssid, sizeof(snapshot.ssid));
    strlcpy(snapshot.hostname, APP_HOSTNAME, sizeof(snapshot.hostname));

    app_state_set_wifi_snapshot(s_config.state, &snapshot);
    app_state_set_wifi_mode(s_config.state, APP_WIFI_CONNECTED);
    DIAG_LOGI("wifi", "Connected to '%s' with IP %s", snapshot.ssid, snapshot.ip[0] != '\0' ? snapshot.ip : "unknown");
    if (s_config.on_wifi_ready != NULL) {
        s_config.on_wifi_ready(s_config.user_ctx);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void) arg;
    (void) event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        app_state_set_wifi_mode(s_config.state, APP_WIFI_CONNECTING);
        DIAG_LOGW("wifi", "WiFi disconnected; retrying '%s'", s_ssid[0] != '\0' ? s_ssid : "unknown");
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        handle_got_ip();
    }
}

esp_err_t wifi_manager_init(const wifi_manager_config_t *config) {
    memset(&s_config, 0, sizeof(s_config));
    s_config = *config;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    s_provisioned = load_credentials() == ESP_OK && s_ssid[0] != '\0';
    app_state_set_provisioned(s_config.state, s_provisioned, !s_provisioned);
    DIAG_LOGI("wifi", "WiFi manager initialized; provisioned=%s", s_provisioned ? "yes" : "no");
    return ESP_OK;
}

bool wifi_manager_is_provisioned(void) {
    return s_provisioned;
}

esp_err_t wifi_manager_start(void) {
    return s_provisioned ? start_station() : start_softap();
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password) {
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &handle), TAG, "open nvs");
    ESP_RETURN_ON_ERROR(nvs_set_str(handle, NVS_KEY_SSID, ssid), TAG, "save ssid");
    ESP_RETURN_ON_ERROR(nvs_set_str(handle, NVS_KEY_PASS, password), TAG, "save pass");
    ESP_RETURN_ON_ERROR(nvs_commit(handle), TAG, "commit nvs");
    nvs_close(handle);

    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, password, sizeof(s_pass));
    s_provisioned = true;
    app_state_set_provisioned(s_config.state, true, false);
    ESP_LOGI(TAG, "Stored WiFi credentials for '%s'", s_ssid);
    DIAG_LOGI("wifi", "Stored credentials for '%s'", s_ssid);
    esp_wifi_stop();
    return start_station();
}

esp_err_t wifi_manager_reset_credentials(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_key(handle, NVS_KEY_SSID);
        nvs_erase_key(handle, NVS_KEY_PASS);
        nvs_commit(handle);
        nvs_close(handle);
    }

    memset(s_ssid, 0, sizeof(s_ssid));
    memset(s_pass, 0, sizeof(s_pass));
    s_provisioned = false;
    app_state_set_provisioned(s_config.state, false, true);
    DIAG_LOGW("wifi", "Cleared stored WiFi credentials");
    esp_wifi_stop();
    return start_softap();
}

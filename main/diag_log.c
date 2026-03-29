#include "diag_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DIAG_LOG_CAPACITY 48

typedef struct {
    SemaphoreHandle_t mutex;
    diag_log_entry_t entries[DIAG_LOG_CAPACITY];
    size_t count;
    size_t head;
    uint32_t next_seq;
    diag_log_listener_t listener;
    void *listener_user_ctx;
    bool initialized;
} diag_log_store_t;

static diag_log_store_t s_diag;

static void diag_log_lock(void) {
    xSemaphoreTake(s_diag.mutex, portMAX_DELAY);
}

static void diag_log_unlock(void) {
    xSemaphoreGive(s_diag.mutex);
}

esp_err_t diag_log_init(void) {
    if (s_diag.initialized) {
        return ESP_OK;
    }

    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_diag.mutex != NULL, ESP_ERR_NO_MEM, "diag_log", "create diagnostics mutex");
    s_diag.initialized = true;
    return ESP_OK;
}

void diag_log_vwrite(diag_log_level_t level, const char *source, const char *fmt, va_list args) {
    if (!s_diag.initialized || s_diag.mutex == NULL || source == NULL || fmt == NULL) {
        return;
    }

    diag_log_entry_t entry_copy = {0};
    diag_log_listener_t listener = NULL;
    void *listener_user_ctx = NULL;

    diag_log_lock();

    diag_log_entry_t *entry = &s_diag.entries[s_diag.head];
    memset(entry, 0, sizeof(*entry));
    entry->seq = ++s_diag.next_seq;
    entry->uptime_ms = esp_log_timestamp();
    entry->level = level;
    strlcpy(entry->source, source, sizeof(entry->source));
    vsnprintf(entry->message, sizeof(entry->message), fmt, args);

    s_diag.head = (s_diag.head + 1) % DIAG_LOG_CAPACITY;
    if (s_diag.count < DIAG_LOG_CAPACITY) {
        s_diag.count++;
    }

    entry_copy = *entry;
    listener = s_diag.listener;
    listener_user_ctx = s_diag.listener_user_ctx;

    diag_log_unlock();

    if (listener != NULL) {
        listener(&entry_copy, listener_user_ctx);
    }
}

void diag_log_write(diag_log_level_t level, const char *source, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diag_log_vwrite(level, source, fmt, args);
    va_end(args);
}

size_t diag_log_copy_recent(diag_log_entry_t *entries, size_t max_entries) {
    if (!s_diag.initialized || s_diag.mutex == NULL || entries == NULL || max_entries == 0) {
        return 0;
    }

    diag_log_lock();

    const size_t copy_count = s_diag.count < max_entries ? s_diag.count : max_entries;
    const size_t skip = s_diag.count - copy_count;
    size_t index = s_diag.count == DIAG_LOG_CAPACITY ? (s_diag.head + skip) % DIAG_LOG_CAPACITY : skip;

    for (size_t i = 0; i < copy_count; ++i) {
        entries[i] = s_diag.entries[index];
        index = (index + 1) % DIAG_LOG_CAPACITY;
    }

    diag_log_unlock();
    return copy_count;
}

void diag_log_set_listener(diag_log_listener_t listener, void *user_ctx) {
    if (!s_diag.initialized || s_diag.mutex == NULL) {
        return;
    }

    diag_log_lock();
    s_diag.listener = listener;
    s_diag.listener_user_ctx = user_ctx;
    diag_log_unlock();
}

const char *diag_log_level_to_string(diag_log_level_t level) {
    switch (level) {
        case DIAG_LOG_LEVEL_INFO:
            return "info";
        case DIAG_LOG_LEVEL_WARN:
            return "warn";
        case DIAG_LOG_LEVEL_ERROR:
            return "error";
    }

    return "info";
}

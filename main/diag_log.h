#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DIAG_LOG_LEVEL_INFO = 0,
    DIAG_LOG_LEVEL_WARN,
    DIAG_LOG_LEVEL_ERROR,
} diag_log_level_t;

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    diag_log_level_t level;
    char source[16];
    char message[96];
} diag_log_entry_t;

typedef void (*diag_log_listener_t)(const diag_log_entry_t *entry, void *user_ctx);

esp_err_t diag_log_init(void);
void diag_log_write(diag_log_level_t level, const char *source, const char *fmt, ...);
void diag_log_vwrite(diag_log_level_t level, const char *source, const char *fmt, va_list args);
size_t diag_log_copy_recent(diag_log_entry_t *entries, size_t max_entries);
void diag_log_set_listener(diag_log_listener_t listener, void *user_ctx);
const char *diag_log_level_to_string(diag_log_level_t level);

#define DIAG_LOGI(source, fmt, ...) diag_log_write(DIAG_LOG_LEVEL_INFO, source, fmt, ##__VA_ARGS__)
#define DIAG_LOGW(source, fmt, ...) diag_log_write(DIAG_LOG_LEVEL_WARN, source, fmt, ##__VA_ARGS__)
#define DIAG_LOGE(source, fmt, ...) diag_log_write(DIAG_LOG_LEVEL_ERROR, source, fmt, ##__VA_ARGS__)

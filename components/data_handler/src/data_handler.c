/**
 * @file data_handler.c
 * @brief Battery monitor orchestrator implementation.
 * 
 * @details
 * Creates the internal 1-deep sample queue, starts the reader/display/telemetry
 * tasks, and implements low-power decisions and deep sleep entry.
 */

#include <inttypes.h>
#include <string.h>
#include "esp_sleep.h"
#include "esp_wifi.h" 
/* Public APIs */
#include "data_handler.h"
#include "display_handler.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "compact_config_db.h"
#include "esp_log.h"

static const char *TAG = "data_handler";
/* single-item queue lives in this translation unit */
static QueueHandle_t s_battery_queue = NULL;
static battery_sample_t s_last_sample;          /* cached last sample */
static volatile uint32_t s_system_errors = 0;   /* aggregated non-sample errors */

/**
 * @brief Initialize internal queue(s) and resources for the monitor.
 *
 * Creates the 1-deep battery sample queue used by telemetry and caches.
 *
 * @param void No parameters.
 * @return void
 */
static void data_handler_queue_init(void);

bool data_handler_publish(const battery_sample_t *s)
{
    if (!s) {
        ESP_LOGW(TAG, "publish called with NULL sample");
        return false;
    }
    if (!s_battery_queue) {
        ESP_LOGW(TAG, "publish before queue init");
        return false;
    }
    /* Merge system-wide errors so consumers see a consistent mask */
    battery_sample_t merged = *s;
    merged.errors |= s_system_errors;
    s_last_sample = merged; /* keep a copy for get_latest() */
    BaseType_t ok = xQueueOverwrite(s_battery_queue, &merged);
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "xQueueOverwrite failed");
        return false;
    }
    ESP_LOGD(TAG, "Sample cached U=%.2fV I=%.1fmA SoC=%u%%", s->voltage_V, s->current_mA, s->soc_percent);
    return true;
}

bool data_handler_wait_for_sample(battery_sample_t *out, TickType_t ticks_to_wait)
{
    if (!out || !s_battery_queue) return false;
    return xQueueReceive(s_battery_queue, out, ticks_to_wait) == pdTRUE;
}

bool data_handler_get_latest(battery_sample_t *out)
{
    if (!out) return false;
    *out = s_last_sample;
    return true;
}

bool data_handler_can_sleep(uint32_t err_flags)
{
    // Skip sleep if any fault is present in either the sample or the system mask
    return (err_flags | s_system_errors) == 0;
}

void data_handler_check_and_sleep(uint32_t errors)
{
    char sleep_en[4] = "0";
    char sleep_sec[12] = "60";

    config_get_str(CONFIG_KEY_SLEEP_ENABLED, sleep_en, sizeof(sleep_en));
    config_get_str(CONFIG_KEY_SLEEP_INTERVAL, sleep_sec, sizeof(sleep_sec));

    if (strcmp(sleep_en, "1") != 0) {
        ESP_LOGI(TAG, "Sleep disabled by config");
        return;
    }

    if ((errors | s_system_errors) != 0) {
        ESP_LOGW(TAG, "Skipping sleep due to active errors: 0x%" PRIX32, errors);
        return;
    }

    int sec = atoi(sleep_sec);
    if (sec <= 0 || sec > 86400) {
        ESP_LOGW(TAG, "Invalid sleep interval: %s", sleep_sec);
        return;
    }

    ESP_LOGI(TAG, "Sleeping for %d seconds", sec);
    display_show_config_message("Sleeping...", "See you soon");
    vTaskDelay(pdMS_TO_TICKS(1000));
    display_sleep(true);
    esp_wifi_stop();
    esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);
    esp_deep_sleep_start();
}

void data_handler_start(void)
{
    /* queue holds the most recent reading only */
    data_handler_queue_init();
}

static void data_handler_queue_init(void)
{
    s_battery_queue = xQueueCreate(1, sizeof(battery_sample_t));
    assert(s_battery_queue && "battery queue alloc failed");
}

void data_handler_set_error(uint32_t bit, bool on)
{
    if (on) {
        s_system_errors |= bit;
    } else {
        s_system_errors &= ~bit;
    }
    ESP_LOGI(TAG, "System error 0x%08" PRIX32 " -> %s (mask=0x%08" PRIX32 ")", (uint32_t)bit, on ? "SET" : "clear", s_system_errors);
}

uint32_t data_handler_get_errors(void)
{
    return s_system_errors;
}

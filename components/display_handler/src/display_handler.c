/**
 * @file display_handler.c
 * @brief OLED display task and helpers.
 * 
 * @details
 *   Owns a private message queue and a FreeRTOS task that renders two-line text
 *   or a battery data frame to the SSD1306 OLED. Provides publisher API and
 *   basic power controls (sleep/clear).
 */

#include <string.h>
#include "display_handler.h"
#include "oled_ssd1306.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "error_defs.h"

#define DISPLAY_QUEUE_DEPTH 4

static QueueHandle_t s_display_queue = NULL;

static const uint32_t FRESH_MS = 10000;
static const char *TAG = "display_handler";
static bool user_configuration_written = false;
static bool persistent_text_mode = false;
/* Cache of last battery sample for revert after timed messages */
static battery_sample_t s_last_battery;
static bool s_have_battery = false;
/* Gate battery rendering during Wi-Fi retry/error sequence to avoid flicker */
static bool s_wifi_error_sequence_active = false;
static bool s_wifi_sequence_pending_flush = false;
static TimerHandle_t s_wifi_seq_timer = NULL;
/* Low battery blink (alternate error text and normal measurements every 3s) */
static bool s_low_batt_blink_active = false;
static bool s_low_batt_show_error = true; /* phase flag */
static TimerHandle_t s_low_batt_timer = NULL;
/* Debounce for stale data to avoid one-off hiccups */
static int s_stale_streak = 0;

/**
 * @brief Timer callback ending deferred Wi-Fi error sequence.
 *
 * @details
 *   Invoked when the deferred timer fires to mark the Wi-Fi error sequence as
 *   complete which allows any cached battery frame to be flushed to the
 *   display queue.
 *
 * @param[in] t FreeRTOS timer handle (unused).
 */
static void display_wifi_seq_timer_cb(TimerHandle_t t);

/**
 * @brief Start low‑battery blink cycle.
 *
 * @details
 *   Creates and arms (if not active) a periodic timer that toggles between
 *   error text and normal measurement view every 3 seconds while low battery
 *   condition persists.
 */
static void display_low_batt_blink_start(void);

/**
 * @brief Stop low‑battery blink cycle.
 *
 * @details
 *   Disarms and deletes the blink timer and resets phase so next start begins
 *   with the error view again.
 */
static void display_low_batt_blink_stop(void);

/**
 * @brief Periodic timer callback toggling low‑battery display phase.
 *
 * @details
 *   Flips between error text and normal metrics while blinking is active and
 *   re-queues the last battery sample for re-render.
 *
 * @param[in] t FreeRTOS timer handle (unused).
 */
static void display_low_batt_timer_cb(TimerHandle_t t);

/**
 * @brief Display task entry point for FreeRTOS.
 *
 * @details
 *   Handles display message queue and renders messages to the OLED.
 *
 * @param[in] arg Unused task argument.
 */
static void display_task(void *arg);

/**
 * @brief Check if battery sample data is fresh.
 *
 * @param[in] s Pointer to battery sample.
 * @return true if data is fresh, false otherwise.
 */
static inline bool display_is_data_fresh(const battery_sample_t *s);

bool display_publish_data(const display_msg_t *m) {
    if (!s_display_queue || !m) return false;
    TickType_t wait = 0;
    if (m->type == DISPLAY_MSG_TEXT) {
        /* We don't really need to block; queue is small and UI messages are rare. */
        wait = 0;
    } else {
        /* Battery messages must never block (produced periodically). */
        wait = 0;
    }
    return xQueueSend(s_display_queue, m, wait) == pdTRUE;
}

void display_sleep(bool enable) {
	oled_ssd1306_sleep(enable);
}

void display_start(void)
{
    if (!s_display_queue) {
        s_display_queue = xQueueCreate(DISPLAY_QUEUE_DEPTH, sizeof(display_msg_t));
        assert(s_display_queue && "Display queue allocation failed");
    }

     /* Increased stack: previous 2048 bytes caused stack protection faults
         during heavy printf/snprintf formatting (float prints are stack hungry). */
    BaseType_t ok = xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
    assert(ok == pdPASS && "Failed to start display task");
}

void display_set_user_configuration_ok(bool status) {
    user_configuration_written = status;
}

// Public display utility for setup_portal
void display_show_config_message(const char *line1, const char *line2) {
    display_msg_t msg = {
        .type = DISPLAY_MSG_TEXT,
        .text = {
            .duration_ms = 0
        }
    };

    // Set line1 and line2 only
    strlcpy(msg.text.line1, line1, sizeof(msg.text.line1) - 1);
    strlcpy(msg.text.line2, line2, sizeof(msg.text.line2) - 1);
	ESP_LOGW("DISPLAY", "SENDING CONFIG MSG: '%s' | '%s'", msg.text.line1, msg.text.line2);
    display_publish_data(&msg);
}

void display_show_timed_message(const char *line1, const char *line2, int duration_ms) {
    if (duration_ms <= 0) {
        display_show_config_message(line1, line2);
        return;
    }
    display_msg_t msg = {
        .type = DISPLAY_MSG_TEXT,
        .text = {
            .duration_ms = duration_ms
        }
    };
    strlcpy(msg.text.line1, line1, sizeof(msg.text.line1) - 1);
    strlcpy(msg.text.line2, line2, sizeof(msg.text.line2) - 1);
    ESP_LOGI(TAG, "Timed message: '%s' | '%s' (%d ms)", msg.text.line1, msg.text.line2, duration_ms);
    display_publish_data(&msg);
}

void display_clear(void) {
    oled_ssd1306_clear();
}

void display_init(void) {
    oled_ssd1306_init();
    oled_ssd1306_clear();
}

void display_set_persistent_text(bool enable) {
    persistent_text_mode = enable;
}

void display_wifi_error_sequence_begin(void) {
    s_wifi_error_sequence_active = true;
    s_wifi_sequence_pending_flush = false;
}

void display_wifi_error_sequence_end(void) {
    s_wifi_error_sequence_active = false;
    if (s_wifi_sequence_pending_flush && s_have_battery && !persistent_text_mode) {
        display_msg_t flush = { .type = DISPLAY_MSG_BATTERY };
        flush.battery = s_last_battery;
        xQueueSendToFront(s_display_queue, &flush, 0);
    }
    s_wifi_sequence_pending_flush = false;
}

void display_wifi_error_sequence_end_deferred(int delay_ms) {
    if (delay_ms <= 0) { display_wifi_error_sequence_end(); return; }
    if (s_wifi_seq_timer) {
        xTimerStop(s_wifi_seq_timer, 0);
        xTimerDelete(s_wifi_seq_timer, 0);
        s_wifi_seq_timer = NULL;
    }
    s_wifi_seq_timer = xTimerCreate("wifi_seq_end", pdMS_TO_TICKS(delay_ms), pdFALSE, NULL, display_wifi_seq_timer_cb);
    if (s_wifi_seq_timer) xTimerStart(s_wifi_seq_timer, 0);
}

static void display_task(void *arg) {

    assert(s_display_queue && "Display queue not initialized");

    display_msg_t rx_msg;

    while (xQueueReceive(s_display_queue, &rx_msg, portMAX_DELAY)) {
        /* Periodically report remaining stack (debug aid) */
        static uint32_t loop_counter = 0;
        if ((loop_counter++ & 0x1F) == 0) { /* every 32 messages */
            UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
            if (hw < 256) {
                ESP_LOGW(TAG, "Low stack watermark: %u bytes", (unsigned)hw);
            } else {
                ESP_LOGD(TAG, "Stack HW mark: %u", (unsigned)hw);
            }
        }
        ESP_LOGI(TAG, "Received message of type %d", rx_msg.type);

    if (rx_msg.type == DISPLAY_MSG_TEXT) {
            // Allow config messages even when persistent_text_mode is enabled
            oled_ssd1306_clear();

            if (strlen(rx_msg.text.line1) > 0) {
                ESP_LOGI(TAG, "Draw line1: '%s'", rx_msg.text.line1);
                oled_ssd1306_draw_text(0, 0, rx_msg.text.line1);
            }

            if (strlen(rx_msg.text.line2) > 0) {
                ESP_LOGI(TAG, "Draw line2: '%s'", rx_msg.text.line2);
                oled_ssd1306_draw_text(0, 1, rx_msg.text.line2);
            }

            /* Auto-revert after timed transient messages (duration_ms > 0).
               During initial provisioning messages use duration_ms = 0 so they stay. */
            if (rx_msg.text.duration_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(rx_msg.text.duration_ms));
                if (s_have_battery) {
                    /* Avoid reverting to a placeholder sample that could still be all zeros (e.g. before first INA reading) */
                    bool is_placeholder = (s_last_battery.voltage_V == 0.0f && s_last_battery.current_mA == 0.0f && s_last_battery.soc_percent == 0 && s_last_battery.errors == 0);
                    if (!is_placeholder) {
                        display_msg_t revert = { .type = DISPLAY_MSG_BATTERY };
                        revert.battery = s_last_battery;
                        revert.battery.timestamp = xTaskGetTickCount();
                        xQueueSendToFront(s_display_queue, &revert, 0);
                        ESP_LOGI(TAG, "Reverting to battery view after timed message (cached sample)");
                    } else {
                        ESP_LOGW(TAG, "Cached battery sample looks placeholder; deferring battery revert");
                    }
                } else {
                    ESP_LOGW(TAG, "No cached battery sample yet; skipping revert");
                }
            }
    } else if (rx_msg.type == DISPLAY_MSG_BATTERY) {
            if (persistent_text_mode) {
                ESP_LOGI(TAG, "Persistent text mode active; skipping battery display");
                continue;
            }
            if (s_wifi_error_sequence_active) {
                /* Cache only; defer draw until sequence ends to prevent flicker */
                s_last_battery = rx_msg.battery;
                s_have_battery = true;
                s_wifi_sequence_pending_flush = true;
                ESP_LOGD(TAG, "Battery frame deferred (Wi-Fi error sequence active)");
                continue;
            }

            /* Measure queue->render latency: from sample timestamp to draw complete */
            TickType_t t_render_start = xTaskGetTickCount();
            uint32_t t_queue_ms = (uint32_t)((t_render_start - rx_msg.battery.timestamp) * portTICK_PERIOD_MS);

            oled_ssd1306_clear();

            battery_sample_t *s = &rx_msg.battery;
            /* Update cache */
            s_last_battery = *s;
            s_have_battery = true;
            char line[32];

            if (!display_is_data_fresh(s)) {
                if (++s_stale_streak >= 2) {
                    ESP_LOGW(TAG, "Stale sample, skipping render (debounced)");
                }
                // Optionally show a subtle stale indicator or skip draw
                continue;
            } else {
                s_stale_streak = 0;
            }

            bool batt_removed = err_test(s->errors, ERR_BATT_REMOVED);
            bool low_batt = !batt_removed && (err_test(s->errors, ERR_LOW_BAT) || err_test(s->errors, ERR_CRITIC_LOW_BAT));
            if (low_batt) {
                /* Ensure blinking timer active */
                display_low_batt_blink_start();
            } else {
                /* Stop blinking if previously active */
                display_low_batt_blink_stop();
            }

            if (batt_removed) {
                oled_ssd1306_draw_text(0,0, "NO BATTERY");
                oled_ssd1306_draw_text(0,1, "INSERT CELL");
                snprintf(line, sizeof line, "U:%4.2fV", s->voltage_V);
                oled_ssd1306_draw_text(0,2, line);
            } else if (low_batt && s_low_batt_show_error) {
                /* Error phase */
                if (err_test(s->errors, ERR_CRITIC_LOW_BAT)) {
                    snprintf(line, sizeof line, "%u%%", s->soc_percent);
                    oled_ssd1306_draw_text(0,0, line);
                    oled_ssd1306_draw_text(0,1, "CRITICALLY LOW!");
                    oled_ssd1306_draw_text(0,2, "REPLACE!");
                } else {
                    snprintf(line, sizeof line, "%u%%", s->soc_percent);
                    oled_ssd1306_draw_text(0,0, line);
                    oled_ssd1306_draw_text(0,1, "BATTERY LOW!");
                    oled_ssd1306_draw_text(0,2, "CHARGE!");
                }
            } else if (err_test(s->errors, ERR_OVERCURR)) {
                oled_ssd1306_draw_text(0,0, "OVERCURRENT!");
            } else if (err_test(s->errors, ERR_INA_OVF)) {
                oled_ssd1306_draw_text(0,0, "INA219 OVERFLOW!");
            } else {
                /* Format using fewer floating prints to save stack/time */
                snprintf(line, sizeof line, "U:%4.2fV", s->voltage_V);
                oled_ssd1306_draw_text(0,0, line);
                snprintf(line, sizeof line, "I:%4.2fmA", s->current_mA);
                oled_ssd1306_draw_text(0,1, line);
                snprintf(line, sizeof line, "%u%%", s->soc_percent);
                oled_ssd1306_draw_text(0,2, line);

                /* End-to-end (sample->display) latency approximation */
                TickType_t t_render_end = xTaskGetTickCount();
                uint32_t t_total_ms = (uint32_t)((t_render_end - rx_msg.battery.timestamp) * portTICK_PERIOD_MS);
                uint32_t t_draw_ms  = (uint32_t)((t_render_end - t_render_start) * portTICK_PERIOD_MS);
                ESP_LOGI(TAG, "Latency: sample->queue=%ums, draw=%ums, sample->display=%ums",
                         (unsigned)t_queue_ms, (unsigned)t_draw_ms, (unsigned)t_total_ms);
            }
        }
    }
}

static inline bool display_is_data_fresh(const battery_sample_t *s) {
	return (xTaskGetTickCount() - s->timestamp) <= pdMS_TO_TICKS(FRESH_MS);
}

static void display_wifi_seq_timer_cb(TimerHandle_t t) {
    (void)t;
    display_wifi_error_sequence_end();
}

static void display_low_batt_blink_start(void) {
    if (s_low_batt_blink_active) return;
    s_low_batt_blink_active = true;
    s_low_batt_show_error = true; /* start by showing error */
    if (s_low_batt_timer) {
        xTimerStop(s_low_batt_timer, 0);
        xTimerDelete(s_low_batt_timer, 0);
        s_low_batt_timer = NULL;
    }
    s_low_batt_timer = xTimerCreate("low_batt", pdMS_TO_TICKS(3000), pdTRUE, NULL, display_low_batt_timer_cb);
    if (s_low_batt_timer) xTimerStart(s_low_batt_timer, 0);
}

static void display_low_batt_blink_stop(void) {
    if (!s_low_batt_blink_active) return;
    if (s_low_batt_timer) {
        xTimerStop(s_low_batt_timer, 0);
        xTimerDelete(s_low_batt_timer, 0);
        s_low_batt_timer = NULL;
    }
    s_low_batt_blink_active = false;
    s_low_batt_show_error = true;
}

static void display_low_batt_timer_cb(TimerHandle_t t) {
    (void)t;
    /* Toggle phase */
    s_low_batt_show_error = !s_low_batt_show_error;
    /* Re-render using cached sample if available and not masked by other modes */
    if (s_low_batt_blink_active && s_have_battery && !persistent_text_mode && !s_wifi_error_sequence_active) {
        display_msg_t m = { .type = DISPLAY_MSG_BATTERY };
        m.battery = s_last_battery;
        xQueueSendToFront(s_display_queue, &m, 0);
    }
}
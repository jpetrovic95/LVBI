/**
 * @file display_handler.h
 * @brief Display (OLED) public API and message format.
 * @ingroup monitor
 * @details
 *   Declares the minimal display API. The queue and message transport are private
 *   to the display task. Callers construct a display_msg_t and publish via
 *   battery_display_publish(), or use higher-level helpers exposed here.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "error_defs.h"

/**
 * @brief Snapshot of battery measurement values.
 * @details Populated by the reader task from INA219 sensor data.
 */
typedef struct {
    float      voltage_V;        /**< Bus voltage in volts */
    float      current_mA;       /**< Load current in milliamps */
    float      power_mW;         /**< Calculated power in milliwatts */
    float      shunt_voltage_mV; /**< Shunt voltage in millivolts */
    uint8_t    soc_percent;      /**< Estimated state of charge (%) */
    err_flag_t errors;           /**< Error flag bit mask */
    TickType_t timestamp;        /**< Tick count when sampled */
} battery_sample_t;

typedef enum {
    DISPLAY_MSG_BATTERY = 0,
    DISPLAY_MSG_TEXT
} display_msg_type_t;

/**
 * @brief Message sent to the display task queue.
 * @details Carries either a battery sample (for numeric view) or up to three
 *          text lines plus an optional duration for transient messages.
 */
typedef struct {
    display_msg_type_t type; /**< Message variant */
    union {
        battery_sample_t battery; /**< Sample payload when type = DISPLAY_MSG_BATTERY */
        struct {
            char line1[32]; /**< First text line */
            char line2[32]; /**< Second text line */
            char line3[32]; /**< Third text line (unused currently) */
            int  duration_ms; /**< Hold time for text message; 0 = persistent */
        } text;
    };
} display_msg_t;

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief Publish a display message to the display task.
 *
 * Non-blocking enqueue; returns false if the task/queue is not ready.
 *
 * @param[in] m (const display_msg_t*) Message to display.
 * @return bool True if enqueued, false otherwise.
 */
bool display_publish_data(const display_msg_t *m);

/**
 * @brief Start the display task and initialize the OLED.
 *
 * Creates the internal message queue and the FreeRTOS task.
 */
void display_start(void);

/**
 * @brief Clear the OLED display.
 */
void display_clear(void);

/**
 * @brief Initialize the OLED display (idempotent).
 *
 * Safe to call multiple times; re-initializes controller state if required.
 */
void display_init(void);

/**
 * @brief Enter/exit OLED sleep mode.
 *
 * @param[in] enable True to sleep; false to wake.
 */
void display_sleep(bool enable);

/**
 * @brief Show a provisioning/config message (two lines).
 *
 * @param[in] line1 First line of text.
 * @param[in] line2 Second line of text.
 */
void display_show_config_message(const char *line1, const char *line2);

/**
 * @brief Show a transient (timed) message, then revert to battery view.
 *
 * @param[in] line1 First line of text.
 * @param[in] line2 Second line of text.
 * @param[in] duration_ms How long to keep the message before auto-revert.
 */
void display_show_timed_message(const char *line1, const char *line2, int duration_ms);

/**
 * @brief Enable/disable persistent text mode.
 *
 * In persistent mode, transient screen clear operations are suppressed.
 *
 * @param[in] enable True to enable persistent text mode.
 */
void display_set_persistent_text(bool enable);

/**
 * @brief Indicate configuration (provisioning) completion status for UI logic.
 * @param[in] status true if configuration finished successfully.
 */
void display_set_user_configuration_ok(bool status);

/**
 * @brief Mark start of Wi-Fi connection / retry error sequence.
 *
 * While active, battery frames are cached but not rendered so that
 * short-lived alternation between Wi-Fi error text and battery state
 * does not cause flicker. Call @ref display_wifi_error_sequence_end
 * after final Wi-Fi status message.
 */
void display_wifi_error_sequence_begin(void);

/**
 * @brief End Wi-Fi error sequence and allow battery frames again.
 *
 * If a cached battery sample exists it is rendered immediately.
 */
void display_wifi_error_sequence_end(void);

/**
 * @brief Schedule end of Wi-Fi error sequence after a delay.
 *
 * Use when posting a final timed offline-mode message; prevents battery
 * frames from appearing until message duration elapses.
 *
 * @param delay_ms Milliseconds to wait before ending the sequence.
 */
void display_wifi_error_sequence_end_deferred(int delay_ms);

#ifdef __cplusplus
}
#endif
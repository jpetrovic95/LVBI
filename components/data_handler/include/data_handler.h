/**
 * @file data_handler.h
 * @brief Battery monitor orchestrator public API.
 * @ingroup monitor
 * @details
 *   Declares the public entry points for the LVBI monitor. The battery monitor
 *   coordinates the reader task (INA219 sampling), the display task (OLED),
 *   and telemetry task (optional cloud publishing). Also provides helpers for
 *   low-power policy and deep sleep.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"  /* for TickType_t */
#include "error_defs.h"
#include "display_handler.h"  /* battery_sample_t */

#ifdef __cplusplus
extern "C" {
#endif

/* battery_sample_t is defined in display_handler.h */

/**
 * @brief Start the battery monitor subsystem.
 *
 * Creates the 1‑deep sample queue and starts reader, display and telemetry tasks.
 */
void data_handler_start(void);

/**
 * @brief Publish (overwrite) the latest battery sample.
 *
 * The internal queue has depth 1; the newest sample atomically replaces the previous.
 *
 * @param[in] s Pointer to sample to publish.
 * @return true on success, false if queue not ready or invalid argument.
 */
bool data_handler_publish(const battery_sample_t *s);

/**
 * @brief Block until a new sample arrives (or timeout).
 *
 * @param[out] out Destination for received sample.
 * @param[in]  ticks_to_wait Maximum ticks to wait (e.g. portMAX_DELAY).
 * @return true if a sample was received; false on timeout or invalid args.
 */
bool data_handler_wait_for_sample(battery_sample_t *out, TickType_t ticks_to_wait);

/**
 * @brief Copy the most recent sample without blocking the queue.
 *
 * @param[out] out Destination for cached sample.
 * @return true on success, false if @p out is NULL.
 */
bool data_handler_get_latest(battery_sample_t *out);

/**
 * @brief Decide if deep sleep is allowed for the current error state.
 *
 * @param[in] err_flags Bit mask of active errors.
 * @return true if no blocking errors are present, else false.
 */
bool data_handler_can_sleep(uint32_t err_flags);

/**
 * @brief Indicate that user configuration (provisioning) is complete.
 *
 * Forwarded to the display module so it can return to the battery view.
 *
 * @param[in] status True if configuration is OK / stored.
 */
void display_set_user_configuration_ok(bool status);

/**
 * @brief Evaluate policy and enter deep sleep if permitted.
 *
 * Reads configurable sleep enable/interval keys and applies error gating.
 *
 * @param[in] errors Current error bit mask.
 */
void data_handler_check_and_sleep(uint32_t errors);

/**
 * @brief Set or clear a system-wide error bit.
 *
 * Modules can report faults that are not tied to a single battery sample
 * (e.g., Wi-Fi offline, display HW failure). The monitor aggregates these
 * bits and ORs them into every published sample. Sleep policy also consults
 * the aggregated mask.
 *
 * @param[in] bit  Error bit from err_flag_t (e.g., ERR_WIFI_LOST).
 * @param[in] on   True to set, false to clear.
 */
void data_handler_set_error(uint32_t bit, bool on);

/**
 * @brief Get the current aggregated system error mask.
 * @return Bit mask of active errors accumulated by the monitor.
 */
uint32_t data_handler_get_errors(void);

#ifdef __cplusplus
}
#endif
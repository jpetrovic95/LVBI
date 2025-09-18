/**
 * @file battery_reader.h
 * @brief Battery reader task public API.
 * @ingroup monitor
 * @details
 *   Declares the entry point for the INA219 sampling task.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the INA219 sampling (battery reader) task.
 * @details Periodically updates voltage/current/power and publishes a
 *          populated battery_sample_t to the monitor + display.
 */
void battery_reader_start(void);

#ifdef __cplusplus
}
#endif
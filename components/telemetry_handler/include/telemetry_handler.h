/**
 * @file battery_telemetry.h
 * @brief Battery telemetry task public API.
 * @ingroup monitor
 * @details
 *   Declares the entry point for the telemetry FreeRTOS task and client resources.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the telemetry task (Ubidots publisher).
 * @details Initializes HTTP client (if token configured) and creates the
 *          FreeRTOS task that waits for fresh samples.
 */
void telemetry_handler_start(void);

#ifdef __cplusplus
}
#endif
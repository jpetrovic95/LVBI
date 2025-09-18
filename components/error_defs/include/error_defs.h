/**
 * @file error_defs.h
 * @brief Error definitions for battery monitoring component.
 * @ingroup errors
 * @details
 *   Handles error definitions and helpers for the battery monitoring system.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Error / warning bit flags (non-overlapping per subsystem).
 */
typedef enum {
    ERR_NONE           = 0,        /**< Helper constant for clearing all */
    ERR_LOW_BAT        = 1 << 0,   /**< State of charge <= 20% */
    ERR_CRITIC_LOW_BAT = 1 << 1,   /**< State of charge <= 5% */
    ERR_INA_OVF        = 1 << 2,   /**< INA219 overflow / invalid raw */
    ERR_I2C_BUS        = 1 << 3,   /**< Any i2c_master_* failure */
    ERR_SENSOR_STALE   = 1 << 4,   /**< No fresh sample within freshness window */
    ERR_COMM_NET       = 1 << 5,   /**< 3 consecutive telemetry POST failures */
    ERR_DISPLAY_HW     = 1 << 6,   /**< SSD1306 command failure */
    ERR_WIFI_LOST      = 1 << 7,   /**< Wi-Fi disconnected > threshold */
    ERR_NVS_CFG        = 1 << 8,   /**< Config blob missing / invalid */
    ERR_OVERCURR       = 1 << 9,   /**< Current exceeded design limit */
    ERR_BATT_REMOVED   = 1 << 10,  /**< Battery physically removed / open circuit detected */
    NUMBER_OF_ERRORS   = 0         /**< Placeholder (not a flag) */
} err_flag_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set an error flag.
 *
 * @param[in,out] m Pointer to error flag variable.
 * @param[in]     b Error flag bit to set.
 */
static inline void err_set   (err_flag_t *m, uint32_t b) { *m |=  b; }

/**
 * @brief Clear an error flag.
 *
 * @param[in,out] m Pointer to error flag variable.
 * @param[in]     b Error flag bit to clear.
 */
static inline void err_clear (err_flag_t *m, uint32_t b) { *m &= ~b; }

/**
 * @brief Test if an error flag is set.
 *
 * @param[in] m Error flag variable.
 * @param[in] b Error flag bit to test.
 * @return true if the flag is set, false otherwise.
 */
static inline bool err_test  (err_flag_t  m, uint32_t b) { return m & b; }

#ifdef __cplusplus
}
#endif
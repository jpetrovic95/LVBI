/**
 * @file ina219.h
 * @brief Driver for the INA219 sensor to monitor voltage and current.
 * @ingroup drivers
 * @details
 *   Initializes the device (ADC settings, averaging), restores calibration, and
 *   provides a mutex-protected readout routine for consistent snapshots.
 */

#pragma once

#include <stdint.h>
#include <esp_err.h>

#include "driver/i2c.h"
#include "error_defs.h"
/**
 * @brief INA219 I2C Definitions
 */
#define INA219_I2C_ADDRESS          0x40    /*!< Default I2C Address */
#define INA219_I2C_TIMEOUT_MS       1000    /*!< Timeout for I2C communication in milliseconds */

/**
 * @brief Conversion factors
 */
#define INA219_VOLTAGE_MULTIPLIER   0.004   /*!< 4mV per bit for bus voltage */
#define INA219_CURRENT_MULTIPLIER   0.01    /*!< 10uA per bit for current */

/**
 * @brief INA219 configuration parameters (runtime + persisted).
 */
typedef struct {
    float shunt_resistance;     /**< Shunt resistor value in ohms (e.g. 0.1) */
    float max_expected_current; /**< Maximum expected current in amps (e.g. 3.2) */
    const char *i2c_device_name; /**< I2C device name as registered with hwa_i2c */        /**< I2C port used for communication */
} ina219_config_t;

/**
 * @brief Latest INA219 measurement snapshot.
 */
typedef struct {
    float voltage_V;
    float current_mA;
    float power_mW;
    float shunt_voltage_mV;
    uint8_t battery_percentage;
    err_flag_t error_flags;
} ina219_readings_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize INA219 (register + configure + calibrate).
 *
 * @param[in] enable_averaging Enable ADC averaging mode for lower noise.
 * @return ESP_OK on success or error code.
 */
esp_err_t ina219_init(bool enable_averaging);

/**
 * @brief Obtain a thread-safe snapshot of the latest readings.
 * @return Pointer to immutable snapshot (copied under mutex).
 */
const ina219_readings_t *ina219_get_readings(void);

/**
 * @brief Update all sensor channels (voltage, current, power, shunt).
 * @return ESP_OK on full success else first failing esp_err_t.
 */
esp_err_t ina219_update_all_readings(void);

/**
 * @brief Switch ADC averaging mode on/off.
 *
 * @param[in] enable_averaging True to enable averaging.
 * @return ESP_OK or error code.
 */
esp_err_t ina219_enable_averaging_adc_mode(bool enable_averaging);

/**
 * @brief Get accumulated error flags for the sensor.
 * @return Bit mask of err_flag_t.
 */
err_flag_t ina219_get_error_flags(void);

#ifdef __cplusplus
}
#endif
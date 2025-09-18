/**
 * @file hwa_i2c.h
 * @brief I2C hardware abstraction public API.
 * @ingroup drivers
 * @details
 *   Declares helpers to initialize the bus, register devices, and perform
 *   read/write operations with retry and error mapping to esp_err_t.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief Initialize the I2C master bus (idempotent).
 *
 * @param port Hardware I2C port index.
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE if already initialized;
 *         or other esp_err_t codes on failure.
 */
esp_err_t hwa_i2c_bus_init(uint8_t port);

/**
 * @brief Register an I2C device by logical name and 7‑bit address.
 *
 * @param name Null-terminated device name (e.g. "ina219").
 * @param address 7-bit address.
 * @return ESP_OK on success; ESP_ERR_NO_MEM if table full; ESP_ERR_INVALID_ARG
 *         on duplicate name; other esp_err_t on low-level failure.
 */
esp_err_t hwa_i2c_register_device(const char *name, uint8_t address);

/**
 * @brief Write raw bytes to a registered device.
 *
 * Retries internally with small backoff.
 *
 * @param name Registered device name.
 * @param data Data buffer.
 * @param len  Number of bytes.
 * @return ESP_OK on success or esp_err_t code.
 */
esp_err_t hwa_i2c_write(const char *name, const uint8_t *data, size_t len);

/**
 * @brief Read raw bytes from a registered device.
 *
 * @param name Registered device name.
 * @param data Destination buffer.
 * @param len  Number of bytes to read.
 * @return ESP_OK on success or error code.
 */
esp_err_t hwa_i2c_read(const char *name, uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
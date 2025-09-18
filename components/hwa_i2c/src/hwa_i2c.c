/**
 * @file hwa_i2c.c
 * @brief I2C HAL implementation (ESP-IDF i2c_master wrapper).
 * @details
 *   Wraps i2c_master API with device registration and convenience read/write
 *   functions, including basic retry and parameter validation.
 *   Designed to be reusable across multiple components.
 */

#include "hwa_i2c.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>

/**
 * @brief I2C configuration parameters
 */
#define HWA_I2C_MAX_DEVICE_NUM                8
#define HWA_I2C_CLIENT_SCL_IO                 5                   /*!< GPIO number for I2C master clock */
#define HWA_I2C_CLIENT_SDA_IO                 4                   /*!< GPIO number for I2C master data  */
#define HWA_I2C_CLIENT_FREQ_HZ                200000               /*!< I2C master clock frequency */

static const char *TAG = "HWA_I2C";

typedef struct {
    char name[16];
    uint8_t address;
    i2c_master_dev_handle_t handle;
} hwa_i2c_device_t;

static i2c_master_bus_handle_t bus_handle = NULL;
static hwa_i2c_device_t devices[HWA_I2C_MAX_DEVICE_NUM];
static size_t device_count = 0;
static SemaphoreHandle_t i2c_mutex; /* global bus mutex */
static int s_i2c_fail_streak = 0;   /* debounce transient failures */

/**
 * @brief Find a registered I2C device by name.
 *
 * @details
 *   Searches the internal device list for a device matching the given name.
 *
 * @param[in] name Device name string.
 * @return Device handle if found, NULL otherwise.
 */
static i2c_master_dev_handle_t hwa_i2c_find_device(const char *name);

esp_err_t hwa_i2c_bus_init(uint8_t port) {
    if (bus_handle != NULL) return ESP_OK; // already initialized

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = port,
        .scl_io_num = HWA_I2C_CLIENT_SCL_IO,
        .sda_io_num = HWA_I2C_CLIENT_SDA_IO,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "i2c_new_master_bus failed");

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    if (!i2c_mutex) {
        i2c_mutex = xSemaphoreCreateMutex();
        assert(i2c_mutex && "Failed to create I2C mutex");
    }

    ESP_LOGI(TAG, "I2C bus initialized.");
    return ESP_OK;
}

esp_err_t hwa_i2c_register_device(const char *name, uint8_t address) {
    if (device_count >= HWA_I2C_MAX_DEVICE_NUM) return ESP_ERR_NO_MEM;
    if (bus_handle == NULL) return ESP_ERR_INVALID_STATE;

    // Check for duplicate names
    for (size_t i = 0; i < device_count; i++) {
        if (strcmp(devices[i].name, name) == 0) {
            ESP_LOGW(TAG, "Device '%s' already registered", name);
            return ESP_ERR_INVALID_ARG;
        }
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = HWA_I2C_CLIENT_FREQ_HZ
    };

    i2c_master_dev_handle_t handle;
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device '%s': %s", name, esp_err_to_name(err));
        return err;
    }

    strlcpy(devices[device_count].name, name, sizeof(devices[device_count].name));
    devices[device_count].address = address;
    devices[device_count].handle = handle;
    device_count++;

    ESP_LOGI(TAG, "Registered device '%s' at address 0x%02X", name, address);
    return ESP_OK;
}

esp_err_t hwa_i2c_write(const char *name, const uint8_t *data, size_t len) {
	//ESP_LOGI(TAG, "Looking up device '%s'", name);
    if (bus_handle == NULL) return ESP_ERR_INVALID_STATE;

    // Find the device
    i2c_master_dev_handle_t handle = NULL;
    handle = hwa_i2c_find_device(name);

    if (handle == NULL) {
        ESP_LOGE(TAG, "Device '%s' not found", name);
        return ESP_ERR_NOT_FOUND;
    }
	
	//retry logic
	esp_err_t err = ESP_FAIL;
	const int max_retries = 3;
	int attempts = 0;
	
    while (attempts < max_retries) {
        if (i2c_mutex) xSemaphoreTake(i2c_mutex, portMAX_DELAY);
        err = i2c_master_transmit(handle, data, len, pdMS_TO_TICKS(200));
        if (i2c_mutex) xSemaphoreGive(i2c_mutex);
        if (err == ESP_OK) break; /* success */

        /* For NACK / invalid state we can optionally try a small bus reset in future */
        ESP_LOGW(TAG, "I2C write failed to '%s' (attempt %d): %s", name, attempts + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(50 + attempts * 50)); /* incremental backoff */
        attempts++;
    }
	
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C write to '%s' ultimately failed: %s", name, esp_err_to_name(err));
        if (++s_i2c_fail_streak >= 2) {
            /* keep local streak; caller can act on return code */
        }
    } else {
        s_i2c_fail_streak = 0;
    /* clear local streak on success */
    }

    return err;
}

esp_err_t hwa_i2c_read(const char *name, uint8_t *data, size_t len) {
    i2c_master_dev_handle_t handle = hwa_i2c_find_device(name);
    if (!handle) {
		ESP_LOGE(TAG, "Device '%s' not found", name);
        return ESP_ERR_NOT_FOUND;
    }
	esp_err_t err = i2c_master_receive(handle, data, len, -1);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to read from device '%s': %s", name, esp_err_to_name(err));
        if (++s_i2c_fail_streak >= 2) {
            /* keep local streak; caller can act on return code */
        }
	} else {
        s_i2c_fail_streak = 0;
    /* clear local streak on success */
	}
    return err;
}

static i2c_master_dev_handle_t hwa_i2c_find_device(const char *name) {
    i2c_master_dev_handle_t handle = NULL;
    for (size_t i = 0; i < device_count; ++i) {
        if (strcmp(name, devices[i].name) == 0) {
            handle = devices[i].handle;
            break;
        }
    }
    return handle;
}

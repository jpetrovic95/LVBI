/**
 * @file lvbi_kv.h
 * @brief Compact KV store public API.
 * @ingroup storage
 * @details
 *   Declares TLV+CRC based key/value storage with SPIFFS/NVS backing.
 *   Provides typed API for string, int32, float, and factory reset.
 */

#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
/* ---- lifecycle ------------------------------------------------------------ */
/**
 * @brief Open the LVBI KV store, mounting SPIFFS and initializing the database.
 *
 * @details
 *   Call once at boot to mount the SPIFFS partition and initialize the key/value store.
 *
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
esp_err_t lvbi_kv_open(void);     /* call once at boot; mounts SPIFFS, inits DB */

/**
 * @brief Close the LVBI KV store file handle.
 *
 * @details
 *   Optional cleanup function to close the file handle for the KV store.
 */
void      lvbi_kv_close(void);    /* optional – close file handle */

/* ---- simple typed API ----------------------------------------------------- */
/**
 * @brief Set a string value for a key in the KV store.
 *
 * @details
 *   Stores the given string value under the specified key in the persistent KV store.
 *
 * @param[in] key Key name (null-terminated string).
 * @param[in] val Value to store (null-terminated string).
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
esp_err_t lvbi_kv_set_str (const char *key, const char *val);

/**
 * @brief Retrieve a string value for a key from the KV store.
 *
 * @details
 *   Looks up the specified key and copies the value to the output buffer.
 *
 * @param[in]  key Key name (null-terminated string).
 * @param[out] out Output buffer for value (null-terminated string).
 * @param[in]  max Maximum length of output buffer.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
esp_err_t lvbi_kv_get_str (const char *key, char *out, size_t max);


/* ---- factory-reset snapshot ---------------------------------------------- */
/**
 * @brief Save a factory-reset snapshot of the KV store to NVS.
 *
 * @details
 *   Stores the first image of the KV store in an NVS blob for factory restore.
 *
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
esp_err_t lvbi_kv_factory_save   (void);   /* store first image in NVS blob */
/**
 * @brief Restore the KV store from the factory-reset snapshot in NVS.
 *
 * @details
 *   Overwrites the current KV store file with the factory image from NVS.
 *
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
esp_err_t lvbi_kv_factory_restore(void);   /* restore & overwrite file     */

#ifdef __cplusplus
}
#endif
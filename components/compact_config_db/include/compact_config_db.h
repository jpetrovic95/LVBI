/**
 * @file compact_config_db.h
 * @brief Configuration abstraction layer (facade over lvbi_kv backend).
 * @ingroup storage
 * @details
 *   Provides a stable, migration‑friendly API backed by the lightweight
 *   lvbi_kv key–value store. Central place for:
 *     - Key name constants (single source of truth)
 *     - Typed helpers (bool / numeric defaults)
 *     - Battery config aggregation + normalization (see battery_cfg_t)
 *     - Schema version hook & snapshotting (future migrations)
 *
 *   Use config_init() instead of calling lvbi_kv_* directly so backend changes
 *   and migrations remain transparent. Application code should include ONLY
 *   this header (not lvbi_kv.h).
 */

#pragma once

#include "lvbi_kv.h"   /* backend */
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h> /* for strcasecmp */

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_KEY_SLEEP_ENABLED   "sleep_en"    // bool ("1" or "0")
#define CONFIG_KEY_SLEEP_INTERVAL  "sleep_sec"   // stringified int (e.g. "60")

/* Frequently used keys (centralized) */
#define CFG_KEY_WIFI_SSID          "wifi_ssid"
#define CFG_KEY_WIFI_PASS          "wifi_pass"
#define CFG_KEY_UBI_TOKEN          "ubidots_token"
#define CFG_KEY_UBI_DEVICE         "ubidots_device"
#define CFG_KEY_BATT_TYPE          "batt_type"
#define CFG_KEY_BATT_FULL          "batt_full"
#define CFG_KEY_BATT_NOMINAL       "batt_nominal"
#define CFG_KEY_BATT_CUTOFF        "batt_cutoff"
#define CFG_KEY_BATT_CAPACITY      "batt_capacity"
#define CFG_KEY_BATT_LUT           "batt_lut"
#define CFG_KEY_BATT_RINT          "batt_rint"
#define CFG_KEY_I_MAX              "i_max"
#define CFG_KEY_SHUNT              "shunt"
#define CFG_KEY_DD_MEASURE         "dd_measure"
#define CFG_KEY_SCHEMA_VERSION     "schema_ver"

#define CONFIG_SCHEMA_VERSION_CURRENT 1

/**
 * @brief Initialize the config database (alias for lvbi_kv_open).
 *
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
#define config_db_init            lvbi_kv_open

/**
 * @brief Set a string value in the config database.
 *
 * @param[in] key Key name.
 * @param[in] val Value string.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
#define config_set_str            lvbi_kv_set_str

/**
 * @brief Get a string value from the config database.
 *
 * @param[in]  key Key name.
 * @param[out] out Output buffer.
 * @param[in]  max Maximum length.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
#define config_get_str            lvbi_kv_get_str

/**
 * @brief Take a snapshot of the current config database state.
 *
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
#define config_factory_snapshot   lvbi_kv_factory_save
/**
 * @brief Restore the config database to its factory state.
 *
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
#define config_factory_restore    lvbi_kv_factory_restore

/* -------------------------------------------------------------------------- */
/* Higher-level helpers (inline)                                              */
/* -------------------------------------------------------------------------- */
typedef struct {
	char  type[16];
	float full;
	float nominal;
	float cutoff;
	int   capacity_mAh;
	float rint_ohm; /* optional, not required by all code paths */
} battery_cfg_t;

/**
 * @brief Set a 32-bit integer value in the config database.
 * @param key Key name.
 * @param val Integer value.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
static inline esp_err_t config_set_i32(const char *key, int32_t val)
{
	char buf[16]; snprintf(buf, sizeof(buf), "%ld", (long)val);
	return config_set_str(key, buf);
}

/**
 * @brief Get a 32-bit integer value from the config database.
 * @param key Key name.
 * @param out Output pointer.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
static inline esp_err_t config_get_i32(const char *key, int32_t *out)
{
	char buf[16]; esp_err_t r = config_get_str(key, buf, sizeof(buf));
	if (r == ESP_OK) *out = strtol(buf, NULL, 10);
	return r;
}

/**
 * @brief Set a float value in the config database.
 * @param key Key name.
 * @param val Float value.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */

static inline esp_err_t config_set_float(const char *key, float val)
{
	char buf[32]; snprintf(buf, sizeof(buf), "%f", val);
	return config_set_str(key, buf);
}
/**
 * @brief Get a float value from the config database.
 * @param key Key name.
 * @param out Output pointer.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */

static inline esp_err_t config_get_float(const char *key, float *out)
{
	char buf[32]; esp_err_t r = config_get_str(key, buf, sizeof(buf));
	if (r == ESP_OK) *out = strtof(buf, NULL);
	return r;
}

 /**
 * @brief Get a boolean configuration value with fallback.
 * @details
 *   Reads the raw string value stored under the given key and interprets it
 *   as a boolean. Accepted TRUE tokens (case-insensitive): "1", "true", "on".
 *   Accepted FALSE tokens: "0", "false", "off". If the key is missing,
 *   empty or contains an unrecognized token, the provided default value
 *   (def_val) is returned.
 * @param key      Key name (null‑terminated string).
 * @param def_val  Default value to return on missing/unrecognized content.
 * @return Parsed boolean or def_val when not present / invalid.
 */
static inline bool config_get_bool(const char *key, bool def_val)
{
	char tmp[8] = {0};
	if (config_get_str(key, tmp, sizeof(tmp)) != ESP_OK || tmp[0] == '\0')
		return def_val;
	if (strcasecmp(tmp, "1") == 0 || strcasecmp(tmp, "true") == 0 || strcasecmp(tmp, "on") == 0) return true;
	if (strcasecmp(tmp, "0") == 0 || strcasecmp(tmp, "false") == 0 || strcasecmp(tmp, "off") == 0) return false;
	return def_val;
}

/**
 * @brief Convenience setter for boolean values (stored as "1"/"0").
 * @param key Key name.
 * @param v Value to write.
 * @return ESP_OK on success, error code otherwise.
 */
static inline esp_err_t config_set_bool(const char *key, bool v)
{
	return config_set_str(key, v ? "1" : "0"); 
}

/**
 * @brief Get float value or default if missing/error.
 * @param key Key name.
 * @param def_val Default value if not found or retrieval fails.
 * @return Retrieved float or def_val.
 */
static inline float config_get_float_def(const char *key, float def_val)
{ 
	float f=0; 
	return (config_get_float(key,&f)==ESP_OK)?f:def_val; 
}

/**
 * @brief Get int32 value or default if missing/error.
 * @param key Key name.
 * @param def_val Default value if not found.
 * @return Retrieved or default value.
 */
static inline int32_t config_get_i32_def(const char *key, int32_t def_val)
{
	int32_t v=0; 
	return (config_get_i32(key,&v)==ESP_OK)?v:def_val;
}

/**
 * @brief Load battery configuration from KV and enforce invariants.
 * @param[out] out Destination structure (ignored if NULL).
 * @note See battery_cfg_t for normalization rules.
 */
static inline void config_load_battery_cfg(battery_cfg_t *out)
{
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	config_get_str(CFG_KEY_BATT_TYPE, out->type, sizeof(out->type));
	if (!out->type[0]) strcpy(out->type, "CUSTOM");
	out->full        = config_get_float_def(CFG_KEY_BATT_FULL, 4.2f);
	out->nominal     = config_get_float_def(CFG_KEY_BATT_NOMINAL, 3.6f);
	out->cutoff      = config_get_float_def(CFG_KEY_BATT_CUTOFF, 3.0f);
	out->capacity_mAh= (int)config_get_i32_def(CFG_KEY_BATT_CAPACITY, 32);
	out->rint_ohm    = config_get_float_def(CFG_KEY_BATT_RINT, 0.0f);
	if (out->full < out->nominal) out->full = 4.2f;
	if (out->cutoff < 2.5f || out->cutoff > out->nominal) out->cutoff = 3.0f;
}

/**
 * @brief Initialize / migrate schema version and snapshot if upgraded.
 */
static inline void config_schema_init(void)
{
	int32_t ver = config_get_i32_def(CFG_KEY_SCHEMA_VERSION, 0);
	if (ver < CONFIG_SCHEMA_VERSION_CURRENT) {
		/* future migrations can go here */
		config_set_i32(CFG_KEY_SCHEMA_VERSION, CONFIG_SCHEMA_VERSION_CURRENT);
		config_factory_snapshot();
	}
}

/**
 * @brief Unified initialization: open backend and apply schema migrations.
 * @note Call this instead of separate config_db_init()+config_schema_init().
 */
static inline esp_err_t config_init(void)
{
	esp_err_t err = config_db_init();
	if (err != ESP_OK) return err;
	config_schema_init();
	return ESP_OK;
}

/**
 * @brief Lightweight existence check (non-alloc, non-owning).
 * @param key Key name.
 * @return true if key exists and has non-empty value.
 */
static inline bool config_key_exists(const char *key)
{
	char tmp[2] = {0};
	return (config_get_str(key, tmp, sizeof(tmp)) == ESP_OK) && tmp[0] != '\0';
}

#ifdef __cplusplus
}
#endif
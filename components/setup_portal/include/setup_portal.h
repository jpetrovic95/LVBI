/**
 * @file setup_portal.h
 * @brief Wi-Fi provisioning portal public API.
 * @ingroup provisioning
 * @details
 *   Declares start/stop of captive portal and helpers to query provisioning state.
 */
#pragma once

#include <stdbool.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start captive portal HTTP server (idempotent).
 */
void setup_portal_start(void);

/**
 * @brief Stop portal and release server resources (no-op if not running).
 */
void setup_portal_stop(void);

/**
 * @brief Test if a provisioning key exists in persistent storage.
 * @param key Key string.
 * @return true if key found, else false.
 */
bool setup_portal_config_key_exists(const char *key);

#ifdef __cplusplus
}
#endif

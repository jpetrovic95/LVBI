/**
 * @file telemetry_handler.c
 * @brief  Telemetry task for Ubidots publishing.
 * 
 * @details
 * Waits for battery samples from the monitor, formats payload and publishes to
 * Ubidots via HTTPS. Implements retry/backoff and basic error reporting.
*/

#include "esp_http_client.h"
/* esp_crt_bundle_attach is provided by esp-tls when CRT bundle enabled in sdkconfig */
#include "esp_crt_bundle.h"
#include "data_handler.h"
#include "telemetry_handler.h"
#include "error_defs.h"
#include "esp_log.h"
#include "compact_config_db.h"

static const char *TAG = "telemetry_handler";
esp_http_client_handle_t http_handle = NULL;

/**
 * @brief Initialize the HTTP client for telemetry.
 *
 * @details
 *   Sets up the HTTP client and configures headers for Ubidots publishing.
 */
static void telemetry_handler_http_init(void);

/**
 * @brief Send a JSON payload via HTTP POST to Ubidots.
 *
 * @param[in] json Pointer to JSON string.
 * @param[in] len  Length of JSON string.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
static esp_err_t telemetry_handler_http_post(const char *json, size_t len);

/**
 * @brief Telemetry FreeRTOS task entry point.
 *
 * @details
 *   Blocks waiting for samples, builds JSON payload, transmits via HTTPS.
 *
 * @param[in] arg Unused task argument.
 */
static void telemetry_handler_task(void *arg);

void telemetry_handler_start(void)
{
    telemetry_handler_http_init(); /* once */
    ESP_LOGI(TAG, "Starting telemetry task for Ubidots...");
    /* Create the telemetry task with a stack size of 4 KB and priority 6 */
    xTaskCreate(telemetry_handler_task, "bat_telemetry", 4 * 1024, NULL, 6, NULL);
}

static void telemetry_handler_http_init(void)
{
    char token[128] = {0};
    char device[64] = "battery";  // fallback default
    config_get_str("ubidots_token", token, sizeof(token));
    config_get_str("ubidots_device", device, sizeof(device));

    if (token[0] == '\0') {
        ESP_LOGW(TAG, "Ubidots token not configured, skipping HTTP init.");
        return;
    }

    /* Static so the pointer given to esp_http_client remains valid */
    static char url[192];
    snprintf(url, sizeof(url), "https://industrial.api.ubidots.com/api/v1.6/devices/%s", device);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_POST,
        .transport_type = HTTP_TRANSPORT_OVER_SSL
    };

    http_handle = esp_http_client_init(&cfg);
    esp_http_client_set_header(http_handle, "X-Auth-Token", token);
    esp_http_client_set_header(http_handle, "Content-Type", "application/json");
}

/* ---------- 2. Send one JSON payload ------------------------------- */
static esp_err_t telemetry_handler_http_post(const char *json, size_t len)
{
    if (!http_handle) return ESP_ERR_INVALID_STATE;   /* handle not ready */
    esp_http_client_set_post_field(http_handle, json, len);
    esp_err_t err = esp_http_client_perform(http_handle);

    if (err == ESP_OK) {
        int code = esp_http_client_get_status_code(http_handle);
        if (code == 200 || code == 201)
            return ESP_OK;
        ESP_LOGW(TAG, "Unexpected HTTP status %d", code);
        return ESP_FAIL;
    }
    return err;
}

static void telemetry_handler_task(void *arg)
{
    char token[128] = {0};
    config_get_str("ubidots_token", token, sizeof(token));
    bool ubidots_enabled = token[0] != '\0';
    if (!ubidots_enabled) {
        ESP_LOGW(TAG, "Ubidots token not configured. Running in local-only mode.");
    }

    const TickType_t fresh_ms = 1000;
    uint8_t fail_streak = 0;

    while (true) {
        battery_sample_t rx;
        if (!data_handler_wait_for_sample(&rx, portMAX_DELAY)) {
            continue; /* queue not ready yet */
        }
        /* Discard stale samples */
        if ((xTaskGetTickCount() - rx.timestamp) > pdMS_TO_TICKS(fresh_ms)) continue;

        // If Ubidots is enabled, build + send payload
        if (ubidots_enabled) {
            /* Deep discharge telemetry toggle: if disabled and SoC == 0 -> skip sending */
            char dd_flag[4] = {0};
            bool dd_enabled = (config_get_str("dd_measure", dd_flag, sizeof(dd_flag)) == ESP_OK && dd_flag[0] == '1');
            // Skip sending telemetry if deep discharge is disabled and SoC is at or below 5%
            if (!dd_enabled && rx.soc_percent == 0 && rx.voltage_V < 1.0f) {
                continue; /* do not publish this (and following 0% samples) */
            }

            char body[196];
            int len = snprintf(body, sizeof body,
                "{\"voltage\":%.3f,\"current\":%.3f,\"soc\":%u,\"err_flags\":%u}",
                rx.voltage_V, rx.current_mA,
                rx.soc_percent, rx.errors);

            TickType_t t_http_start = xTaskGetTickCount();
            esp_err_t e = telemetry_handler_http_post(body, len);
            TickType_t t_http_end   = xTaskGetTickCount();
            if (e == ESP_OK) {
                ESP_LOGI(TAG, "Ubidots POST successful: %s", body);
                uint32_t http_ms = (uint32_t)((t_http_end - t_http_start) * portTICK_PERIOD_MS);
                uint32_t end_to_end_ms = (uint32_t)((t_http_end - rx.timestamp) * portTICK_PERIOD_MS);
                ESP_LOGI(TAG, "Latency: HTTP=%ums, sample->telemetry=%ums",
                            (unsigned)http_ms, (unsigned)end_to_end_ms);
                fail_streak = 0;
                rx.errors &= ~ERR_COMM_NET;           /* reflect in this sample */
                data_handler_set_error(ERR_COMM_NET, false); /* clear global */
            } else if (++fail_streak >= 3) {
                rx.errors |= ERR_COMM_NET;           /* reflect in this sample */
                data_handler_set_error(ERR_COMM_NET, true);  /* set global */
                fail_streak = 0;
                ESP_LOGW(TAG, "POST failed: %s", esp_err_to_name(e));
            }
        }

        // Sleep decision happens regardless of Ubidots; include global errors
        uint32_t agg = rx.errors | data_handler_get_errors();
        if (data_handler_can_sleep(agg)) {
            data_handler_check_and_sleep(agg);
        }
    }
}



#include <string.h>
#include <inttypes.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "data_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "display_handler.h"
#include "telemetry_handler.h"
#include "battery_reader.h"
#include "hwa_i2c.h"
#include "ina219.h"
#include "setup_portal.h"
#include "compact_config_db.h"
#include "error_defs.h"

/* -------------------------------------------------------------------------- */
/* Static function prototypes                                                 */
/* -------------------------------------------------------------------------- */
/**
 * @brief Return short textual description for a Wi-Fi disconnect reason code.
 * @param reason Wi-Fi disconnect reason (WIFI_REASON_* constant).
 * @return Pointer to static string description.
 */
static const char *wifi_reason_desc(uint8_t reason);
/**
 * @brief Start device in SoftAP configuration mode.
 * @return ESP_OK on success or error code from Wi-Fi stack.
 */
static esp_err_t wifi_start_softap(void);
/**
 * @brief Start STA mode connection attempt sequence.
 * Attempts connection using stored SSID/PASS with retry policy.
 * @return ESP_OK if connected (IP obtained) else ESP_FAIL.
 */
static esp_err_t wifi_start_sta(void);
/**
 * @brief Event handler invoked on successful IP acquisition.
 * @param arg User context (unused).
 * @param base Event base (IP_EVENT).
 * @param id Event id (IP_EVENT_STA_GOT_IP).
 * @param data Event data (ip_event_got_ip_t*).
 * @return void
 */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data);
/**
 * @brief Event handler for WIFI_EVENT_STA_START.
 * @param arg User context (unused).
 * @param base Event base (WIFI_EVENT).
 * @param event_id Event id (WIFI_EVENT_STA_START).
 * @param event_data Event data pointer (unused).
 * @return void
 */
static void on_wifi_sta_start(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
/**
 * @brief Event handler for STA disconnections (retry policy and display updates).
 * @param arg User context (unused).
 * @param base Event base (WIFI_EVENT).
 * @param event_id Event id (WIFI_EVENT_STA_DISCONNECTED).
 * @param event_data Disconnect event data (wifi_event_sta_disconnected_t*).
 * @return void
 */
static void on_wifi_disconnected(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
/**
 * @brief Background task: monitors BOOT button for long-press to start portal.
 * @param arg Unused task parameter.
 * @return void
 */
static void boot_button_monitor_task(void *arg);
/**
 * @brief Update boot count stored in NVS (for diagnostics).
 * @return void
 */
static void update_boot_count(void);

#define BIT_IP_READY       BIT0
#define BIT_DISCONNECTED   BIT1
// Requirement: attempt Wi-Fi up to 3 times (disconnects/timeouts) then give up silently (battery display only)
#define MAX_WIFI_RETRIES   3
// BOOT button long-press (active LOW) handling
#define BOOT_BUTTON_GPIO      9
#define BOOT_LONG_PRESS_MS    3000

static const char *TAG = "MAIN";
static const uint8_t i2c_monitoring_port = 0u;
static EventGroupHandle_t s_net_event_group;
static int wifi_retry_count = 0; // counts failed attempts
static int last_disconnect_reason = 0;
static bool portal_started = false;
static esp_netif_t *sta_netif = NULL;
static bool monitor_started = false;
static TaskHandle_t s_main_task = NULL;
static esp_netif_t *ap_netif  = NULL;



/**
 * @brief Application entry point.
 *
 * Initializes NVS, network stack, configuration database, I2C / sensors,
 * display, and handles Wi-Fi connection attempts with fallback to offline mode.
 */
void app_main(void)
{
    s_main_task = xTaskGetCurrentTaskHandle();
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", reason);
    
    ESP_ERROR_CHECK(nvs_flash_init());
    update_boot_count();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(config_init());

    // Check for first-time setup (no saved SSID)
    if (!setup_portal_config_key_exists(CFG_KEY_WIFI_SSID)) {
        display_set_user_configuration_ok(false); // set status flag
        data_handler_set_error(ERR_NVS_CFG, true);
        hwa_i2c_bus_init(i2c_monitoring_port);
        ina219_init(true); // ensure sensor active even during initial portal
        display_init();
        display_sleep(false);
        display_set_persistent_text(true); // keep config text fixed
        display_start();
        vTaskDelay(pdMS_TO_TICKS(150));
        // Start battery monitoring early so we can still show live data when config text not persistent anymore
        data_handler_start();
        battery_reader_start();
        display_show_config_message("LVBI Configuration", "Setup via SoftAP");
        ESP_LOGI(TAG, "No configuration found. Starting setup portal...");
        portal_started = true;
        ESP_ERROR_CHECK(wifi_start_softap());
        setup_portal_start();
    vTaskSuspend(NULL);  // halt execution
    }

    // Only now create STA netif since the device is not in portal mode
    sta_netif = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(hwa_i2c_bus_init(i2c_monitoring_port));
    ina219_init(true); // Enable averaging mode
    display_init();
    display_sleep(false);
    ESP_LOGI(TAG, "Starting display task...");
    vTaskDelay(pdMS_TO_TICKS(80));
    display_start();
    // Start battery monitoring BEFORE Wi-Fi so transient Wi-Fi errors can revert to battery data
    data_handler_start();
    battery_reader_start();
    // Start BOOT button monitor EARLY (before Wi-Fi connects) so user can request portal any time
    if (!monitor_started) {
        if (xTaskCreate(boot_button_monitor_task, "btn_mon", 3072, NULL, 5, NULL) == pdPASS) {
            monitor_started = true;
        } else {
            ESP_LOGE(TAG, "Failed to create boot button monitor task");
        }
    }
    // Read stored config
    char ssid[33] = "", pass[65] = "", token[128] = "";
    config_get_str(CFG_KEY_WIFI_SSID, ssid, sizeof(ssid));
    config_get_str(CFG_KEY_WIFI_PASS, pass, sizeof(pass));
    config_get_str(CFG_KEY_UBI_TOKEN, token, sizeof(token));
    bool ubidots_enabled = token[0] != '\0';
    bool has_ssid = ssid[0] != '\0';
    ESP_LOGI(TAG, "SSID: %s", has_ssid ? ssid : "(none)");
    ESP_LOGI(TAG, "Credentials present: %s", has_ssid ? "yes" : "no");
    ESP_LOGI(TAG, "Ubidots token set: %s", ubidots_enabled ? "yes" : "no");

    if (!ubidots_enabled) {
        ESP_LOGW(TAG, "No Ubidots token found. Running in local-only mode.");
    }

    bool wifi_connected = false;

    if (has_ssid && !portal_started) {
        /* Suppress battery frames while Wi-Fi error / retry messages occur */
        display_wifi_error_sequence_begin();
        wifi_retry_count = 0;
        last_disconnect_reason = 0;
        ESP_LOGI(TAG, "Attempting STA connection: try 1/%d", MAX_WIFI_RETRIES);
        if (wifi_start_sta() == ESP_OK) {
            wifi_connected = true;
            /* Show battery data immediately */
            display_wifi_error_sequence_end();
        } else {
            ESP_LOGW(TAG, "Wi-Fi connection failed after %d attempts; continuing offline", wifi_retry_count);
            /* Keep sequence active until offline message completes (timed) */
        }
    }

    // After Wi-Fi attempts (success or not) decide display/telemetry state (no auto portal except first-time case)
    if (wifi_connected) {
        display_set_user_configuration_ok(true); // config exists and Wi-Fi OK
        display_set_persistent_text(false);
        data_handler_set_error(ERR_NVS_CFG, false);
    } else {
        // Config exists but Wi-Fi unreachable -> still mark configuration OK so battery view cycles
        display_set_user_configuration_ok(true);
        display_set_persistent_text(false);
        data_handler_set_error(ERR_NVS_CFG, false);
        // Optionally show a short transient error only once (omit repetitive messages)
        const int offline_msg_ms = 3000;
        display_show_timed_message("Wi-Fi Error", "Offline mode", offline_msg_ms);
        /* End gating after offline message expires so first battery frame (or error) appears cleanly */
        display_wifi_error_sequence_end_deferred(offline_msg_ms + 50);
    }

    // Telemetry only if Wi-Fi connected (portal does not start telemetry; offline skip)
    if (wifi_connected && !portal_started) {
        telemetry_handler_start();
    } else if (portal_started) {
        ESP_LOGI(TAG, "Portal active -> skipping telemetry start");
    } else {
        ESP_LOGI(TAG, "Wi-Fi not connected -> telemetry disabled this cycle");
    }

    if (!ubidots_enabled) {
        ESP_LOGI(TAG, "No Ubidots. Manual sleep check.");
        data_handler_check_and_sleep(0);
    }

    ESP_LOGI(TAG, "Battery monitoring running on I2C %d", i2c_monitoring_port);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) data;

        ESP_LOGI("MAIN", "Wi-Fi connected");
        ESP_LOGI("MAIN", "GOT IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI("MAIN", "Subnet mask: " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI("MAIN", "Gateway:     " IPSTR, IP2STR(&event->ip_info.gw));

        /* Signal the event group so wifi_start_sta() can continue */
        xEventGroupSetBits(s_net_event_group, BIT_IP_READY);
        /* Clear Wi-Fi lost error */
        data_handler_set_error(ERR_WIFI_LOST, false);
    }
}

static void on_wifi_sta_start(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START -> esp_wifi_connect()");
        esp_wifi_connect();
    }
}

static void on_wifi_disconnected(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    if (portal_started) {
        ESP_LOGI(TAG, "STA disconnect ignored (portal active)");
        return;
    }
    wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
    last_disconnect_reason = disc->reason;
    wifi_retry_count++;
    const char *desc = wifi_reason_desc(disc->reason);
    char msg[48];
    snprintf(msg, sizeof(msg), "%s (retry %d)", desc, wifi_retry_count);
    ESP_LOGW(TAG, "Wi-Fi STA disconnected, reason: %d", disc->reason);
    if (wifi_retry_count <= MAX_WIFI_RETRIES) {
        display_show_timed_message("Wi-Fi Error", msg, 1000);
        ESP_LOGI(TAG, "Transient Wi-Fi error shown: %s", msg);
    }
    /* Debounce: only mark Wi-Fi lost after >=2 consecutive retries */
    if (wifi_retry_count >= 2) {
        data_handler_set_error(ERR_WIFI_LOST, true);
    }
    if (s_net_event_group) {
        xEventGroupSetBits(s_net_event_group, BIT_DISCONNECTED);
    }
}

static esp_err_t wifi_start_softap(void)
{
    // Ensure any previous instance is torn down
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        esp_wifi_stop();
        esp_wifi_deinit();
    }
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = { 0 };
    strcpy((char*)ap_cfg.ap.ssid, "LVBI-Setup");
    ap_cfg.ap.ssid_len = 10;
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Log AP IP info
    if (ap_netif) {
        esp_netif_ip_info_t ipi;
        if (esp_netif_get_ip_info(ap_netif, &ipi) == ESP_OK) {
            ESP_LOGI(TAG, "SoftAP IP: " IPSTR " Gateway: " IPSTR " Netmask: " IPSTR,
                     IP2STR(&ipi.ip), IP2STR(&ipi.gw), IP2STR(&ipi.netmask));
        }
    }
    return ESP_OK;
}


static esp_err_t wifi_start_sta(void)
{
    if (portal_started) { ESP_LOGW(TAG, "wifi_start_sta ignored (portal active)"); return ESP_FAIL; }

    char ssid[33] = "", pass[65] = "";
    if (config_get_str(CFG_KEY_WIFI_SSID, ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        ESP_LOGW(TAG, "No SSID. Launching portal.");
        return ESP_FAIL;
    }
    config_get_str(CFG_KEY_WIFI_PASS, pass, sizeof(pass));

    // Clean any previous instance
    wifi_mode_t current_mode;
    if (esp_wifi_get_mode(&current_mode) == ESP_OK) {
        esp_wifi_stop();
        esp_wifi_deinit();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t sta_cfg = { 0 };
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    s_net_event_group = xEventGroupCreate();
    esp_event_handler_instance_t ip_handler, discon_handler, sta_start_handler;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, &ip_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_disconnected, NULL, &discon_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_START, on_wifi_sta_start, NULL, &sta_start_handler));

    char connect_msg[48];
    snprintf(connect_msg, sizeof(connect_msg), "Connecting to: %.20s", ssid);
    display_show_config_message("Wi-Fi Setup", connect_msg);
    ESP_ERROR_CHECK(esp_wifi_start());

    const TickType_t attempt_timeout_ms = 8000; // per attempt
    bool success = false;
    while (!portal_started) {
        EventBits_t bits = xEventGroupWaitBits(s_net_event_group, BIT_IP_READY | BIT_DISCONNECTED,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(attempt_timeout_ms));
        if (portal_started) break;
        if (bits & BIT_IP_READY) {
            ESP_LOGI(TAG, "Wi-Fi connected.");
            success = true;
            break;
        }
        if (bits & BIT_DISCONNECTED) {
            if (wifi_retry_count >= MAX_WIFI_RETRIES) {
                ESP_LOGW(TAG, "Too many retries. Giving up Wi-Fi for this cycle.");
                break;
            }
            ESP_LOGI(TAG, "Retrying Wi-Fi connection: SSID='%s', PASS='%s'", ssid, pass);
            ESP_LOGI(TAG, "Triggering esp_wifi_connect()...");
            esp_wifi_connect();
            if (wifi_retry_count >= 2) {
                data_handler_set_error(ERR_WIFI_LOST, true);
            }
            continue;
        }
        // timeout without explicit disconnect
        if (!(bits & (BIT_IP_READY | BIT_DISCONNECTED))) {
            wifi_retry_count++;
            char msg[48];
            snprintf(msg, sizeof(msg), "Timeout (retry %d)", wifi_retry_count);
            display_show_timed_message("Wi-Fi Error", msg, 1000);
            ESP_LOGW(TAG, "Wi-Fi attempt timeout (%d ms)", (int)attempt_timeout_ms);
            if (wifi_retry_count >= MAX_WIFI_RETRIES) {
                ESP_LOGW(TAG, "Too many timeouts. Giving up Wi-Fi for this cycle.");
                break;
            }
            ESP_LOGI(TAG, "Retrying Wi-Fi connection: SSID='%s', PASS='%s'", ssid, pass);
            ESP_LOGI(TAG, "Triggering esp_wifi_connect()...");
            esp_wifi_connect();
            if (wifi_retry_count >= 2) {
                data_handler_set_error(ERR_WIFI_LOST, true);
            }
        }
    }

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);
    esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, discon_handler);
    esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_START, sta_start_handler);
    vEventGroupDelete(s_net_event_group);
    s_net_event_group = NULL;

    if (!success) {
        esp_wifi_stop();
        esp_wifi_deinit();
        data_handler_set_error(ERR_WIFI_LOST, true);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void update_boot_count(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);

    if (err == ESP_OK) {
        int32_t boot_count = 0;
        nvs_get_i32(nvs_handle, "boot_count", &boot_count);
        boot_count++;
        nvs_set_i32(nvs_handle, "boot_count", boot_count);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        ESP_LOGI("MAIN", "Boot count: %" PRId32, boot_count);
    } else {
        ESP_LOGE("MAIN", "Failed to open NVS");
    }
}

static const char *wifi_reason_desc(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND: return "SSID missing";      // 201
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_AUTH_FAIL:   return "Auth failed";
        case WIFI_REASON_ASSOC_FAIL:  return "Assoc failed";
        default:                      return "Disconnect";
    }
}

// Runtime monitor task: detect BOOT long-press at ANY time and start portal
static void boot_button_monitor_task(void *arg) {
    const TickType_t poll = pdMS_TO_TICKS(50);
    ESP_LOGI(TAG, "BOOT monitor task started (GPIO%d, long=%d ms)", BOOT_BUTTON_GPIO, BOOT_LONG_PRESS_MS);
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_cfg);

    enum { BTN_IDLE = 0, BTN_PRESSING } state = BTN_IDLE;
    TickType_t press_start = 0;
    int last_reported_second = -1;
    TickType_t last_debug = 0;

    while (1) {
        if (portal_started) {
            ESP_LOGI(TAG, "Monitor task ending (portal active)");
            vTaskDelete(NULL);
        }

        int level = gpio_get_level(BOOT_BUTTON_GPIO);
        TickType_t now = xTaskGetTickCount();

        // Periodic raw level debug (every 500 ms for visibility)
        if (((now - last_debug) * portTICK_PERIOD_MS) >= 500) {
            last_debug = now;
            ESP_LOGD(TAG, "BOOT raw level=%d state=%d", level, state);
        }

        if (state == BTN_IDLE) {
            if (level == 0) { // transition to pressing
                state = BTN_PRESSING;
                press_start = now;
                last_reported_second = -1; // force immediate UI update
                display_show_config_message("Portal access", "Hold...");
                ESP_LOGI(TAG, "BOOT press start");
            }
        } else { // BTN_PRESSING
            if (level != 0) {
                // Released early
                uint32_t held_ms = (now - press_start) * portTICK_PERIOD_MS;
                ESP_LOGI(TAG, "BOOT released early after %u ms (< %d ms)", (unsigned)held_ms, BOOT_LONG_PRESS_MS);
                state = BTN_IDLE;
            } else {
                uint32_t held_ms = (now - press_start) * portTICK_PERIOD_MS;
                int sec = (int)(held_ms / 1000);
                if (sec != last_reported_second) {
                    last_reported_second = sec;
                    if (held_ms < BOOT_LONG_PRESS_MS) {
                        int remaining = (BOOT_LONG_PRESS_MS - held_ms + 999)/1000; // ceil
                        char line2[24];
                        snprintf(line2, sizeof(line2), "Hold %ds more", remaining);
                        display_show_config_message("Portal access", line2);
                        ESP_LOGI(TAG, "Countdown: %d s remaining", remaining);
                    }
                }
                if (held_ms >= BOOT_LONG_PRESS_MS) {
                    ESP_LOGI(TAG, "BOOT long-press confirmed (%d ms threshold) -> portal", BOOT_LONG_PRESS_MS);
                    portal_started = true;
                    wifi_mode_t mode; if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_STA) {
                        esp_wifi_stop();
                        esp_wifi_deinit();
                    }
                    wifi_start_softap();
                    display_set_user_configuration_ok(false);
                    display_set_persistent_text(true);
                    display_show_config_message("LVBI Configuration", "Runtime BOOT");
                    setup_portal_start();
                    if (s_main_task) {
                        ESP_LOGI(TAG, "Suspending main task (portal mode)");
                        vTaskSuspend(s_main_task);
                    }
                    // Loop will terminate next iteration via portal_started check
                }
            }
        }
        vTaskDelay(poll);
    }
}
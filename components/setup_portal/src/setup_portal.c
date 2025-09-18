/**
 * @file setup_portal.c
 * @brief Wi-Fi provisioning portal implementation.
 * @details
 *   Implements captive portal for first-time Wi-Fi setup, config entry, and reset.
 *   Handles HTTP server, form parsing, and config persistence.
 */

#include "setup_portal.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "compact_config_db.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "setup_portal";
static httpd_handle_t s_server = NULL;


/**
 * @brief HTTP GET handler for the setup portal root page.
 *
 * @details
 *   Serves the main HTML page for Wi-Fi provisioning.
 *
 * @param[in] req HTTP request object.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
static esp_err_t setup_portal_root_get(httpd_req_t *req);

/**
 * @brief HTTP POST handler for configuration reset.
 *
 * @details
 *   Handles requests to reset the configuration to factory defaults.
 *
 * @param[in] req HTTP request object.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
static esp_err_t setup_portal_handle_reset(httpd_req_t *req);

/**
 * @brief URL-decode a string in-place.
 *
 * @details
 *   Decodes percent-encoded characters in the input string.
 *
 * @param[in,out] s String to decode.
 */
static void setup_portal_url_decode(char *s);

/**
 * @brief HTTP POST handler for saving configuration.
 *
 * @details
 *   Parses and saves configuration data from the portal form.
 *
 * @param[in] req HTTP request object.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
static esp_err_t setup_portal_save_post(httpd_req_t *req);
/* ---------------- Handlers ---------------- */

bool setup_portal_config_key_exists(const char *key) {
    char val[2];
    return config_get_str(key, val, sizeof(val)) == ESP_OK;
}

void setup_portal_start(void)
{
    if (s_server) return; /* already running */

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 10;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));
    httpd_uri_t reset_uri = {
    .uri      = "/reset",
    .method   = HTTP_POST,
    .handler  = setup_portal_handle_reset,
    .user_ctx = NULL
    };
    httpd_uri_t root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = setup_portal_root_get,
        .user_ctx = NULL
    };
    httpd_uri_t save = {
        .uri      = "/save",
        .method   = HTTP_POST,
        .handler  = setup_portal_save_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &save);
    httpd_register_uri_handler(s_server, &reset_uri);
    ESP_LOGI(TAG, "Captive portal started");
}

void setup_portal_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Captive portal stopped");
    }
}

static esp_err_t setup_portal_root_get(httpd_req_t *req)
{
    char batt_type[16] = {0};
    char batt_full[16] = {0};
    char batt_nom[16] = {0};
    char batt_cut[16] = {0};
    char batt_cap[16] = {0};
    char batt_lut[256] = {0};
    char batt_rint[16] = {0};

    config_get_str("batt_type", batt_type, sizeof(batt_type));
    config_get_str("batt_full", batt_full, sizeof(batt_full));
    config_get_str("batt_nominal", batt_nom, sizeof(batt_nom));
    config_get_str("batt_cutoff", batt_cut, sizeof(batt_cut));
    config_get_str("batt_capacity", batt_cap, sizeof(batt_cap));
    config_get_str("batt_lut", batt_lut, sizeof(batt_lut));
    config_get_str("batt_rint", batt_rint, sizeof(batt_rint));

    if (!batt_type[0]) strcpy(batt_type, "CUSTOM");
    if (!batt_full[0]) strcpy(batt_full, "4.2");
    if (!batt_nom[0]) strcpy(batt_nom, "3.6");
    if (!batt_cut[0]) strcpy(batt_cut, "3.0");
    if (!batt_cap[0]) strcpy(batt_cap, "32");
    if (!batt_rint[0]) strcpy(batt_rint, "0");

    char html[4096];
    const char *opt_custom_sel = (strncasecmp(batt_type, "CUSTOM", 6)==0)?"selected":"";
    const char *opt_gen_sel = (strncasecmp(batt_type, "GENERIC", 7)==0)?"selected":"";

    /* batt_lut format hint */
    snprintf(html, sizeof(html),
        "<html><head><title>LVBI Setup</title></head><body>"
        "<form method=POST action=/save>"
        "<h3>Wi-Fi Configuration</h3>"
        "SSID: <input name=ssid><br>"
        "Password: <input name=pass type=password><br>"
        "<h3>Ubidots</h3>"
        "Token: <input name=token><br>"
        "Device Label: <input name=ubidots_device value=battery><br>"
        "<h3>Sensor Calibration</h3>"
        "Battery Voltage Range (V): <input name=v_range value=5.0><br>"
        "Max Expected Current (mA): <input name=imax value=50><br>"
        "Shunt Resistance (Ohms): <input name=shunt value=0.1><br>"
        "<p style=\"color:red;\">"
        "INA219: Max V<sub>BUS</sub> = 32 V; Shunt voltage = +/-320mV.<br>"
        "With 0.1 mOhm shunt: safe up to 3.2 A. For higher current use lower-value shunt (update both field & hardware)."
        "</p>"
        "<h3>Battery Parameters</h3>"
        "Battery Type: <select name=batt_type>"
            "<option value=CUSTOM %s>CUSTOM (uses LUT below)</option>"
            "<option value=GENERIC %s>GENERIC (1S Li-Ion preset)</option>"
        "</select><br>"
        "Full Voltage (V): <input name=batt_full value=%s><br>"
        "Nominal Voltage (V): <input name=batt_nominal value=%s><br>"
    "Cutoff Voltage (V): <input name=batt_cutoff value=%s><br>"
        "Capacity (mAh): <input name=batt_capacity value=%s><br>"
        "Internal Resistance Rint (Ohms): <input name=batt_rint value=%s><br>"
        "Custom LUT (V:%% pairs comma-separated, ascending). Example: 3.30:0,3.40:5,3.80:85,4.20:100<br>"
        "<textarea name=batt_lut rows=3 cols=60>%s</textarea><br>"
        "Allow Deep Discharge Telemetry: <input name=dd_measure type=checkbox value=1><br>"
        "<h3>Sleep Settings</h3>"
        "Sleep Enabled: <input name=sleep_en type=checkbox value=1><br>"
        "Sleep Interval (s): <input type=number name=sleep_sec min=1 max=86400 value=60><br><br>"
        "<input type=submit value='Save Configuration'>"
        "</form><br><br>"
        "<form method=POST action=/reset><button type=submit>Reset Configuration</button></form>"
        "</body></html>",
    opt_custom_sel, opt_gen_sel,
        batt_full, batt_nom, batt_cut, batt_cap, batt_rint, batt_lut);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t setup_portal_handle_reset(httpd_req_t *req) {
    ESP_LOGW(TAG, "Web-based factory reset triggered");
    config_factory_restore();
    httpd_resp_sendstr(req, "Device is resetting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* Very small x-www-form-urlencoded parser (no URL‑decode for Unicode) */
static void setup_portal_url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; ++p, ++o) {
        if (*p == '+') {
            *o = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *o = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o = *p;
        }
    }
    *o =
     '\0';
}

static esp_err_t setup_portal_save_post(httpd_req_t *req)
{
    char buf[512] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;

    typedef struct {
        const char *form_key;
        const char *config_key;
    } config_entry_t;

    const config_entry_t config_map[] = {
        {"ssid",            "wifi_ssid"},
        {"pass",            "wifi_pass"},
        {"token",           "ubidots_token"},
        {"ubidots_device",  "ubidots_device"},
        {"v_range",         "v_range"},
        {"imax",            "i_max"},
        {"batt_type",       "batt_type"},
        {"batt_full",       "batt_full"},
        {"batt_nominal",    "batt_nominal"},
    {"batt_cutoff",     "batt_cutoff"},
        {"batt_capacity",   "batt_capacity"},
    {"batt_rint",       "batt_rint"},
        {"batt_lut",        "batt_lut"},
        {"shunt",           "shunt"},
        {"dd_measure",      "dd_measure"},
        {"sleep_en",        CONFIG_KEY_SLEEP_ENABLED},
        {"sleep_sec",       CONFIG_KEY_SLEEP_INTERVAL},
        {"rtc_wakeup_min",  "rtc_wakeup_min"},
    };

    char *tok = strtok(buf, "&");
    while (tok) {
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0';
            char *key = tok;
            char *val = eq + 1;
            setup_portal_url_decode(val);

            bool known = false;
            for (size_t i = 0; i < sizeof(config_map) / sizeof(config_map[0]); ++i) {
                if (!strcmp(key, config_map[i].form_key)) {
                    config_set_str(config_map[i].config_key, val);
                    known = true;
                    break;
                }
            }

            if (!known) {
                ESP_LOGW(TAG, "Unknown config key: %s", key);
            }
        }
        tok = strtok(NULL, "&");
    }

    // Ensure sleep_en has a value, even if checkbox is unchecked
    if (!setup_portal_config_key_exists(CONFIG_KEY_SLEEP_ENABLED)) {
        config_set_str(CONFIG_KEY_SLEEP_ENABLED, "0");
    }

    config_factory_snapshot();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Saved! Rebooting...");
    ESP_LOGI(TAG, "Configuration saved, restarting");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; // not reached
}


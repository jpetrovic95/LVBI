/**
 * @file battery_reader.c
 * @brief INA219 sampling task and sample publication.
 * 
 * @details
 * Periodically polls INA219, fills battery_sample_t, publishes to display and
 * the monitor queue (for telemetry), and handles error flag updates.
 */

#include "data_handler.h"
#include "display_handler.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ina219.h"
#include <string.h>

#define PERIOD_MS 5000           /* 5-s reader period           */

static const char *TAG = "battery_reader";

/**
 * @brief Battery reader FreeRTOS task.
 *
 * @details
 *   Polls INA219 every configured period, computes derived metrics, publishes display updates and the latest sample to the monitor.
 *
 * @param[in] arg Unused task argument.
 */
static void battery_reader_task(void *arg);


void battery_reader_start(void)
{   
    ESP_LOGI(TAG, "Starting battery_reader task...");
    BaseType_t rc = xTaskCreate(battery_reader_task, "battery_reader_task", 3 * 1024, NULL, 3, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create battery_reader_task (rc=%ld)", (long)rc);
    }
}

/**
 * @brief FreeRTOS task to monitor voltage and current every 10 seconds
 *
 */
static void battery_reader_task(void *arg)
{
    battery_sample_t s;


    for (;;) {
        /* ----------------------------------------------------------- */
        /* 1 ─ get fresh INA219 data                                   */
        /* ----------------------------------------------------------- */
        ESP_LOGD(TAG, "Cycle start");
        vTaskDelay(pdMS_TO_TICKS(200));          /* sensor settle before first real sample */
        TickType_t t_sample_start = xTaskGetTickCount();
        ina219_update_all_readings();
        TickType_t t_sample_end = xTaskGetTickCount();
        const ina219_readings_t *r = ina219_get_readings();
        
        if (!r) {                                /* sensor failure    */
            ESP_LOGE(TAG, "INA219 read failed");
            vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
            continue;
        }

        /* ----------------------------------------------------------- */
        /* 2 ─ fill sample struct                                      */
        /* ----------------------------------------------------------- */
        memset(&s, 0, sizeof s);
        s.voltage_V        = r->voltage_V;
        s.current_mA       = r->current_mA;
        s.power_mW         = r->power_mW;
        s.shunt_voltage_mV = r->shunt_voltage_mV;
        s.soc_percent      = r->battery_percentage;
        s.errors           = r->error_flags;     /* start with INA flags */
        s.timestamp        = xTaskGetTickCount();
        uint32_t t_sensor_ms = (uint32_t)((t_sample_end - t_sample_start) * portTICK_PERIOD_MS);
        /* Merge any aggregated system errors so the UI also reflects them */
        s.errors |= data_handler_get_errors();

        /* ----------------------------------------------------------- */
        /* 3 ─ derive battery-level error flags                        */
        /* ----------------------------------------------------------- */
        if (!err_test(s.errors, ERR_BATT_REMOVED)) {
            if (s.soc_percent <= 5) {
                err_set  (&s.errors, ERR_CRITIC_LOW_BAT);
                err_clear(&s.errors, ERR_LOW_BAT);
            } else if (s.soc_percent <= 20) {
                err_set  (&s.errors, ERR_LOW_BAT);
                err_clear(&s.errors, ERR_CRITIC_LOW_BAT);
            } else {
                err_clear(&s.errors, ERR_LOW_BAT | ERR_CRITIC_LOW_BAT);
            }
        } else {
            /* Battery removed – suppress low battery flags */
            err_clear(&s.errors, ERR_LOW_BAT | ERR_CRITIC_LOW_BAT);
        }

        
        /* ----------------------------------------------------------- */
        /* 4 ─ publish the latest sample                               */
        /* ----------------------------------------------------------- */

        if (!data_handler_publish(&s)) {
            ESP_LOGW(TAG, "data_handler_publish failed");
        }
        
        display_msg_t msg = {
            .type = DISPLAY_MSG_BATTERY,
            .battery = s
        };
        
        if (!display_publish_data(&msg)) {
            ESP_LOGW(TAG, "display_publish_data failed");
        } else {
            ESP_LOGD(TAG, "Published sample: U=%.2fV I=%.1fmA SoC=%u%% (sensor=%ums)", s.voltage_V, s.current_mA, s.soc_percent, (unsigned)t_sensor_ms);
        }

        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
    }
}



/**
 * @file oled_ssd1306.h
 * @brief SSD1306 OLED driver public API.
 * @ingroup drivers
 * @details
 *   Declares initialization, clear, draw text, and sleep controls for SSD1306.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the SSD1306 controller (safe to call multiple times).
 * @return ESP_OK on success or esp_err_t code.
 */
esp_err_t oled_ssd1306_init(void);

/**
 * @brief Clear the entire display buffer (fills with 0x00).
 */
void oled_ssd1306_clear(void);

/**
 * @brief Draw a text string at column/page coordinate.
 * @param x Column (0..127)
 * @param y Page row (0..3 for 32px height)
 * @param text Null‑terminated ASCII string (unsupported chars -> space)
 */
void oled_ssd1306_draw_text(uint8_t x, uint8_t y, const char *text);

/**
 * @brief Enter or leave low-power (display off) mode.
 * @param enable True: sleep (off), False: wake (on).
 */
void oled_ssd1306_sleep(bool enable);

#ifdef __cplusplus
}
#endif
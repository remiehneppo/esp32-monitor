#ifndef DISPLAY_SSD1306_H
#define DISPLAY_SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c.h"

// I2C Pin Configurations for OLED display on xh-S3E-AI board
#define I2C_MASTER_SDA_IO           41    // Change to your SDA Pin (e.g., 4 or 18)
#define I2C_MASTER_SCL_IO           42    // Change to your SCL Pin (e.g., 5 or 19)
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000 // 100kHz improves OLED bring-up reliability
#define I2C_MASTER_TX_BUF_DISABLE   0
#define I2C_MASTER_RX_BUF_DISABLE   0

#define SSD1306_I2C_ADDRESS         0x3C  // OLED standard I2C address

#define SSD1306_WIDTH               128
#define SSD1306_HEIGHT              64

/**
 * @brief Set or clear a single pixel in the local frame buffer
 */
void ssd1306_draw_pixel(uint8_t x, uint8_t y, bool on);

/**
 * @brief Initialize SSD1306 I2C Master and LCD configuration
 */
esp_err_t ssd1306_init(void);

/**
 * @brief Clear local frame buffer
 */
void ssd1306_clear(void);

/**
 * @brief Write buffer content to SSD1306 over I2C
 */
esp_err_t ssd1306_update(void);

/**
 * @brief Draw character in the local frame buffer
 * 
 * @param x X coordinate (0 to 127)
 * @param y Y coordinate / Page (0 to 7, each page is 8 pixels high)
 * @param c Character to draw
 */
void ssd1306_draw_char(uint8_t x, uint8_t page, char c);

/**
 * @brief Draw string in the local frame buffer
 * 
 * @param x X coordinate (0 to 127)
 * @param y Page (0 to 7)
 * @param str Null terminated string
 */
void ssd1306_draw_string(uint8_t x, uint8_t page, const char *str);

/**
 * @brief Draw line in the local frame buffer
 */
void ssd1306_draw_line(int x0, int y0, int x1, int y1, bool on);

/**
 * @brief Draw visual bar in the local frame buffer
 * 
 * @param page Page to draw bar (0 to 7)
 * @param start_x Start X coordinate
 * @param end_x End X coordinate
 * @param percentage Percentage filled (0 to 100)
 */
void ssd1306_draw_bar(uint8_t page, uint8_t start_x, uint8_t end_x, uint8_t percentage);

#endif // DISPLAY_SSD1306_H

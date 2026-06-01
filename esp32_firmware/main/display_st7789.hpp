#pragma once

#include "display_interface.hpp"
#include "driver/spi_master.h"
#include "driver/gpio.h"

// Logical canvas dimensions (same as SSD1306 so rendering logic is reused).
#define ST7789_CANVAS_WIDTH   128
#define ST7789_CANVAS_HEIGHT  64

// Physical panel dimensions (landscape orientation).
#define ST7789_PHYS_WIDTH     320
#define ST7789_PHYS_HEIGHT    240

// Scale factor: each logical pixel → NxN physical pixels.
#define ST7789_SCALE          2

/**
 * ST7789 SPI LCD display.
 * Physical resolution: 320×240 (landscape). Logical canvas: 128×64 scaled 2×.
 * Supports full RGB565 color via set_color().
 */
class DisplayST7789 : public IDisplay {
public:
    /**
     * @param cs    Chip Select GPIO
     * @param dc    Data/Command GPIO
     * @param rst   Reset GPIO (use GPIO_NUM_NC to skip hardware reset)
     * @param mosi  MOSI (SDA) GPIO
     * @param sclk  SCLK (SCL) GPIO
     * @param host  SPI host device (SPI2_HOST or SPI3_HOST)
     */
    DisplayST7789(
        gpio_num_t        cs,
        gpio_num_t        dc,
        gpio_num_t        rst,
        gpio_num_t        mosi,
        gpio_num_t        sclk,
        spi_host_device_t host = SPI2_HOST
    );

    ~DisplayST7789() override;

    esp_err_t init() override;
    void      clear() override;
    esp_err_t update() override;
    void      set_color(uint16_t fg, uint16_t bg) override;
    void      draw_pixel(uint8_t x, uint8_t y, bool on) override;
    // draw_char, draw_string, draw_line, draw_bar: use IDisplay default implementations
    uint16_t  width() const override  { return ST7789_CANVAS_WIDTH; }
    uint16_t  height() const override { return ST7789_CANVAS_HEIGHT; }

private:
    gpio_num_t        m_cs;
    gpio_num_t        m_dc;
    gpio_num_t        m_rst;
    gpio_num_t        m_mosi;
    gpio_num_t        m_sclk;
    spi_host_device_t m_host;

    spi_device_handle_t m_spi;

    // RGB565 framebuffer for the logical 128×64 canvas.
    uint16_t m_framebuf[ST7789_CANVAS_WIDTH * ST7789_CANVAS_HEIGHT];

    uint16_t m_fg_color = DISPLAY_COLOR_WHITE;
    uint16_t m_bg_color = DISPLAY_COLOR_BLACK;

    esp_err_t send_cmd(uint8_t cmd);
    esp_err_t send_data(const uint8_t *data, size_t len);
    esp_err_t set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    void      set_pixel_color(uint8_t x, uint8_t y, uint16_t color);
};

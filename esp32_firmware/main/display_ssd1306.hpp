#pragma once

#include "display_interface.hpp"
#include "driver/i2c.h"
#include "driver/gpio.h"

#define SSD1306_DEFAULT_SDA_PIN     GPIO_NUM_41
#define SSD1306_DEFAULT_SCL_PIN     GPIO_NUM_42
#define SSD1306_DEFAULT_I2C_PORT    I2C_NUM_0
#define SSD1306_DEFAULT_FREQ_HZ     100000
#define SSD1306_DEFAULT_I2C_ADDR    0x3C

#define SSD1306_WIDTH   128
#define SSD1306_HEIGHT  64

/**
 * SSD1306 I2C OLED display (128×64 monochrome).
 * Implements IDisplay. set_color() is ignored — always renders white-on-black.
 */
class DisplaySSD1306 : public IDisplay {
public:
    /**
     * @param sda      SDA GPIO pin
     * @param scl      SCL GPIO pin
     * @param i2c_port I2C peripheral number
     * @param freq_hz  I2C clock frequency
     * @param i2c_addr I2C device address (probes 0x3C and 0x3D if init fails)
     */
    DisplaySSD1306(
        gpio_num_t sda      = SSD1306_DEFAULT_SDA_PIN,
        gpio_num_t scl      = SSD1306_DEFAULT_SCL_PIN,
        i2c_port_t i2c_port = SSD1306_DEFAULT_I2C_PORT,
        uint32_t   freq_hz  = SSD1306_DEFAULT_FREQ_HZ,
        uint8_t    i2c_addr = SSD1306_DEFAULT_I2C_ADDR
    );

    esp_err_t init() override;
    void      clear() override;
    esp_err_t update() override;
    void      set_color(uint16_t fg, uint16_t bg) override; // no-op for monochrome
    void      draw_pixel(uint8_t x, uint8_t y, bool on) override;
    // draw_char: overrides base to use direct page-buffer write (faster than draw_pixel loop)
    void      draw_char(uint8_t x, uint8_t y, char c) override;
    // draw_bar: overrides base to use direct page-buffer write (faster than draw_pixel loop)
    void      draw_bar(uint8_t y, uint8_t start_x, uint8_t end_x, uint8_t percentage) override;
    // draw_string, draw_line: use IDisplay default implementations
    uint16_t  width() const override  { return SSD1306_WIDTH; }
    uint16_t  height() const override { return SSD1306_HEIGHT; }

private:
    gpio_num_t  m_sda;
    gpio_num_t  m_scl;
    i2c_port_t  m_i2c_port;
    uint32_t    m_freq_hz;
    uint8_t     m_i2c_addr;

    uint8_t     m_framebuf[SSD1306_WIDTH * (SSD1306_HEIGHT / 8)]; // 1024 bytes

    esp_err_t write_cmd(uint8_t cmd);
    esp_err_t write_data(const uint8_t *data, size_t len);
    esp_err_t probe_address(uint8_t addr);
};

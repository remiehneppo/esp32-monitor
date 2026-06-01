#include "display_ssd1306.hpp"
#include "display_font.hpp"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SSD1306";

DisplaySSD1306::DisplaySSD1306(gpio_num_t sda, gpio_num_t scl,
                               i2c_port_t i2c_port, uint32_t freq_hz,
                               uint8_t i2c_addr)
    : m_sda(sda), m_scl(scl), m_i2c_port(i2c_port),
      m_freq_hz(freq_hz), m_i2c_addr(i2c_addr)
{
    memset(m_framebuf, 0, sizeof(m_framebuf));
}

esp_err_t DisplaySSD1306::write_cmd(uint8_t cmd)
{
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (m_i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, 0x00, true); // Co=0, D/C#=0 → command
    i2c_master_write_byte(h, cmd, true);
    i2c_master_stop(h);
    esp_err_t ret = i2c_master_cmd_begin(m_i2c_port, h, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(h);
    return ret;
}

esp_err_t DisplaySSD1306::write_data(const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (m_i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, 0x40, true); // Co=0, D/C#=1 → data
    i2c_master_write(h, data, len, true);
    i2c_master_stop(h);
    esp_err_t ret = i2c_master_cmd_begin(m_i2c_port, h, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(h);
    return ret;
}

esp_err_t DisplaySSD1306::probe_address(uint8_t addr)
{
    m_i2c_addr = addr;
    return write_cmd(0xAE); // Turn display off — used as a probe
}

esp_err_t DisplaySSD1306::init()
{
    i2c_config_t conf = {};
    conf.mode             = I2C_MODE_MASTER;
    conf.sda_io_num       = m_sda;
    conf.sda_pullup_en    = GPIO_PULLUP_ENABLE;
    conf.scl_io_num       = m_scl;
    conf.scl_pullup_en    = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = m_freq_hz;

    esp_err_t err = i2c_param_config(m_i2c_port, &conf);
    if (err != ESP_OK) return err;

    err = i2c_driver_install(m_i2c_port, conf.mode, 0, 0, 0);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "I2C initialized. Probing SSD1306...");

    const uint8_t candidates[] = {0x3C, 0x3D};
    err = ESP_FAIL;
    for (uint8_t addr : candidates) {
        err = probe_address(addr);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SSD1306 found at 0x%02X", addr);
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 not found on 0x3C or 0x3D");
        return err;
    }

    static const uint8_t init_cmds[] = {
        0xAE,        // Display off
        0xD5, 0x80,  // Clock divide ratio / oscillator
        0xA8, 0x3F,  // Multiplex ratio
        0xD3, 0x00,  // Display offset
        0x40,        // Start line
        0x8D, 0x14,  // Charge pump on
        0x20, 0x00,  // Horizontal addressing mode
        0xA1,        // Segment remap
        0xC8,        // COM scan direction
        0xDA, 0x12,  // COM pins config
        0x81, 0xCF,  // Contrast
        0xD9, 0xF1,  // Pre-charge period
        0xDB, 0x40,  // VCOMH level
        0xA4,        // Entire display on (from RAM)
        0xA6,        // Normal (non-inverted)
        0xAF         // Display on
    };

    for (uint8_t cmd : init_cmds) {
        err = write_cmd(cmd);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Init cmd 0x%02X failed", cmd);
            return err;
        }
    }

    ESP_LOGI(TAG, "SSD1306 configured successfully");
    clear();
    return update();
}

void DisplaySSD1306::clear()
{
    memset(m_framebuf, 0, sizeof(m_framebuf));
}

esp_err_t DisplaySSD1306::update()
{
    esp_err_t err = ESP_OK;
    for (uint8_t page = 0; page < 8; ++page) {
        if ((err = write_cmd(0x21)) != ESP_OK) return err;
        if ((err = write_cmd(0x00)) != ESP_OK) return err;
        if ((err = write_cmd(0x7F)) != ESP_OK) return err;
        if ((err = write_cmd(0x22)) != ESP_OK) return err;
        if ((err = write_cmd(page)) != ESP_OK) return err;
        if ((err = write_cmd(page)) != ESP_OK) return err;
        err = write_data(&m_framebuf[(size_t)page * SSD1306_WIDTH], SSD1306_WIDTH);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

void DisplaySSD1306::set_color(uint16_t /*fg*/, uint16_t /*bg*/)
{
    // Monochrome display — color is ignored.
}

void DisplaySSD1306::draw_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) return;
    size_t idx = (size_t)(y / 8) * SSD1306_WIDTH + x;
    uint8_t mask = (uint8_t)(1U << (y & 7U));
    if (on) m_framebuf[idx] |= mask;
    else    m_framebuf[idx] &= (uint8_t)~mask;
}

void DisplaySSD1306::draw_char(uint8_t x, uint8_t y, char c)
{
    uint8_t page = y / 8;
    if (x > SSD1306_WIDTH - 8 || page > 7) return;
    uint8_t idx = (uint8_t)c;
    if (idx < 32 || idx > 127) idx = 32;
    idx -= 32;
    for (int col = 0; col < 8; col++) {
        m_framebuf[page * SSD1306_WIDTH + x + col] = display_font::font8x8[idx][col];
    }
}

void DisplaySSD1306::draw_bar(uint8_t y, uint8_t start_x, uint8_t end_x, uint8_t percentage)
{
    uint8_t page = y / 8;
    if (start_x >= end_x || end_x >= SSD1306_WIDTH || page > 7) return;
    uint8_t w = end_x - start_x + 1;
    uint8_t filled = (uint8_t)((w * percentage) / 100U);

    for (uint8_t col = 0; col < w; col++) {
        uint8_t pattern;
        if (col == 0 || col == w - 1)  pattern = 0x7E; // border
        else if (col <= filled)        pattern = 0x7E; // filled
        else                           pattern = 0x42; // outline
        m_framebuf[page * SSD1306_WIDTH + start_x + col] = pattern;
    }
}

#include "display_st7789.hpp"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ST7789";

// ST7789 command codes
#define ST7789_SWRESET  0x01
#define ST7789_SLPOUT   0x11
#define ST7789_NORON    0x13
#define ST7789_INVON    0x21
#define ST7789_DISPON   0x29
#define ST7789_CASET    0x2A
#define ST7789_RASET    0x2B
#define ST7789_RAMWR    0x2C
#define ST7789_MADCTL   0x36
#define ST7789_COLMOD   0x3A

// MADCTL bits for landscape (MY=0 MX=1 MV=1 ML=0 BGR=0 MH=0)
#define ST7789_MADCTL_LANDSCAPE  0x70

// Centered canvas offset — not used when stretch-filling the full screen.
// Kept as comments for reference only.
// static constexpr uint16_t CANVAS_X_OFF = (ST7789_PHYS_WIDTH  - ST7789_CANVAS_WIDTH  * ST7789_SCALE) / 2; // 32
// static constexpr uint16_t CANVAS_Y_OFF = (ST7789_PHYS_HEIGHT - ST7789_CANVAS_HEIGHT * ST7789_SCALE) / 2; // 56

DisplayST7789::DisplayST7789(gpio_num_t cs, gpio_num_t dc, gpio_num_t rst,
                             gpio_num_t mosi, gpio_num_t sclk,
                             spi_host_device_t host)
    : m_cs(cs), m_dc(dc), m_rst(rst), m_mosi(mosi), m_sclk(sclk), m_host(host),
      m_spi(nullptr)
{
    memset(m_framebuf, 0, sizeof(m_framebuf));
}

DisplayST7789::~DisplayST7789()
{
    if (m_spi) {
        spi_bus_remove_device(m_spi);
        spi_bus_free(m_host);
        m_spi = nullptr;
    }
}

esp_err_t DisplayST7789::send_cmd(uint8_t cmd)
{
    gpio_set_level(m_dc, 0);
    spi_transaction_t t = {};
    t.length    = 8;
    t.flags     = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = cmd;
    return spi_device_polling_transmit(m_spi, &t);
}

esp_err_t DisplayST7789::send_data(const uint8_t *data, size_t len)
{
    if (len == 0) return ESP_OK;
    gpio_set_level(m_dc, 1);
    spi_transaction_t t = {};
    t.length    = len * 8;
    t.tx_buffer = data;
    return spi_device_polling_transmit(m_spi, &t);
}

esp_err_t DisplayST7789::set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    esp_err_t err;
    uint8_t buf[4];

    buf[0] = x0 >> 8; buf[1] = x0 & 0xFF; buf[2] = x1 >> 8; buf[3] = x1 & 0xFF;
    if ((err = send_cmd(ST7789_CASET)) != ESP_OK) return err;
    if ((err = send_data(buf, 4))      != ESP_OK) return err;

    buf[0] = y0 >> 8; buf[1] = y0 & 0xFF; buf[2] = y1 >> 8; buf[3] = y1 & 0xFF;
    if ((err = send_cmd(ST7789_RASET)) != ESP_OK) return err;
    if ((err = send_data(buf, 4))      != ESP_OK) return err;

    return ESP_OK;
}

void DisplayST7789::set_pixel_color(uint8_t x, uint8_t y, uint16_t color)
{
    if (x >= ST7789_CANVAS_WIDTH || y >= ST7789_CANVAS_HEIGHT) return;
    m_framebuf[(uint16_t)y * ST7789_CANVAS_WIDTH + x] = color;
}

esp_err_t DisplayST7789::init()
{
    // Configure DC and RST as outputs
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << m_dc);
    if (m_rst != GPIO_NUM_NC) io.pin_bit_mask |= (1ULL << m_rst);
    io.mode = GPIO_MODE_OUTPUT;
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;

    // Hardware reset
    if (m_rst != GPIO_NUM_NC) {
        gpio_set_level(m_rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(m_rst, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    // SPI bus
    spi_bus_config_t bus = {};
    bus.mosi_io_num     = m_mosi;
    bus.miso_io_num     = -1;
    bus.sclk_io_num     = m_sclk;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = ST7789_PHYS_WIDTH * sizeof(uint16_t); // one full physical row

    err = spi_bus_initialize(m_host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = 40 * 1000 * 1000; // 40 MHz
    dev.mode           = 0;
    dev.spics_io_num   = m_cs;
    dev.queue_size     = 7;

    err = spi_bus_add_device(m_host, &dev, &m_spi);
    if (err != ESP_OK) return err;

    // ST7789 init sequence
    if ((err = send_cmd(ST7789_SWRESET)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(150));

    if ((err = send_cmd(ST7789_SLPOUT)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t b;

    b = 0x55; // RGB565
    if ((err = send_cmd(ST7789_COLMOD)) != ESP_OK) return err;
    if ((err = send_data(&b, 1))        != ESP_OK) return err;

    b = ST7789_MADCTL_LANDSCAPE;
    if ((err = send_cmd(ST7789_MADCTL)) != ESP_OK) return err;
    if ((err = send_data(&b, 1))        != ESP_OK) return err;

    if ((err = send_cmd(ST7789_INVON))  != ESP_OK) return err;
    if ((err = send_cmd(ST7789_NORON))  != ESP_OK) return err;
    if ((err = send_cmd(ST7789_DISPON)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "ST7789 initialized (canvas %dx%d stretch-filled to %dx%d)",
             ST7789_CANVAS_WIDTH, ST7789_CANVAS_HEIGHT,
             ST7789_PHYS_WIDTH, ST7789_PHYS_HEIGHT);

    clear();
    return update();
}

void DisplayST7789::clear()
{
    uint16_t bg = m_bg_color;
    // Fill with big-endian background color
    uint16_t be_bg = (bg >> 8) | (bg << 8);
    for (size_t i = 0; i < ST7789_CANVAS_WIDTH * ST7789_CANVAS_HEIGHT; i++) {
        m_framebuf[i] = be_bg;
    }
}

esp_err_t DisplayST7789::update()
{
    // Stretch the 128×64 logical canvas to fill the full 320×240 physical display.
    // Each physical pixel (px, py) maps to logical pixel:
    //   lx = px * CANVAS_WIDTH  / PHYS_WIDTH
    //   ly = py * CANVAS_HEIGHT / PHYS_HEIGHT
    esp_err_t err = set_window(0, 0, ST7789_PHYS_WIDTH - 1, ST7789_PHYS_HEIGHT - 1);
    if (err != ESP_OK) return err;

    if ((err = send_cmd(ST7789_RAMWR)) != ESP_OK) return err;

    const size_t line_bytes = ST7789_PHYS_WIDTH * sizeof(uint16_t);
    uint16_t *line_buf = (uint16_t *)heap_caps_malloc(line_bytes, MALLOC_CAP_DMA);
    if (!line_buf) return ESP_ERR_NO_MEM;

    for (uint16_t py = 0; py < ST7789_PHYS_HEIGHT; py++) {
        uint8_t ly = (uint8_t)((uint32_t)py * ST7789_CANVAS_HEIGHT / ST7789_PHYS_HEIGHT);
        const uint16_t *src_row = &m_framebuf[(uint16_t)ly * ST7789_CANVAS_WIDTH];
        for (uint16_t px = 0; px < ST7789_PHYS_WIDTH; px++) {
            uint8_t lx = (uint8_t)((uint32_t)px * ST7789_CANVAS_WIDTH / ST7789_PHYS_WIDTH);
            line_buf[px] = src_row[lx];
        }
        err = send_data((const uint8_t *)line_buf, line_bytes);
        if (err != ESP_OK) {
            heap_caps_free(line_buf);
            return err;
        }
    }

    heap_caps_free(line_buf);
    return ESP_OK;
}

void DisplayST7789::set_color(uint16_t fg, uint16_t bg)
{
    // Store as big-endian for direct framebuffer writes
    m_fg_color = (fg >> 8) | (fg << 8);
    m_bg_color = (bg >> 8) | (bg << 8);
}

void DisplayST7789::draw_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= ST7789_CANVAS_WIDTH || y >= ST7789_CANVAS_HEIGHT) return;
    m_framebuf[(uint16_t)y * ST7789_CANVAS_WIDTH + x] = on ? m_fg_color : m_bg_color;
}

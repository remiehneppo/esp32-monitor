#pragma once

#include "display_interface.hpp"
#include "sdkconfig.h"

#if CONFIG_DISPLAY_ST7789
  #include "display_st7789.hpp"
#else
  #include "display_ssd1306.hpp"
#endif

/**
 * Factory function: construct and return the display instance selected
 * at compile time via Kconfig (Component config → Display Configuration).
 *
 * The caller owns the returned pointer.
 */
static inline IDisplay *create_display()
{
#if CONFIG_DISPLAY_ST7789
    return new DisplayST7789(
        (gpio_num_t)CONFIG_ST7789_PIN_CS,
        (gpio_num_t)CONFIG_ST7789_PIN_DC,
        (gpio_num_t)CONFIG_ST7789_PIN_RST,
        (gpio_num_t)CONFIG_ST7789_PIN_MOSI,
        (gpio_num_t)CONFIG_ST7789_PIN_SCLK
    );
#else
    return new DisplaySSD1306(); // uses default I2C pins (SDA=41, SCL=42)
#endif
}

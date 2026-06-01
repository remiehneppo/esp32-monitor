#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// RGB565 color constants for color displays
#define DISPLAY_COLOR_BLACK   ((uint16_t)0x0000)
#define DISPLAY_COLOR_WHITE   ((uint16_t)0xFFFF)
#define DISPLAY_COLOR_CYAN    ((uint16_t)0x07FF)
#define DISPLAY_COLOR_GREEN   ((uint16_t)0x07E0)
#define DISPLAY_COLOR_YELLOW  ((uint16_t)0xFFE0)
#define DISPLAY_COLOR_MAGENTA ((uint16_t)0xF81F)
#define DISPLAY_COLOR_RED     ((uint16_t)0xF800)
#define DISPLAY_COLOR_GRAY    ((uint16_t)0x8410)

/**
 * Abstract display interface. Each display type implements this class.
 *
 * Coordinate system:
 *   - x: 0 to (width-1)  in pixels
 *   - y: 0 to (height-1) in pixels — used by ALL draw calls
 *     draw_char/draw_string/draw_bar treat y as the top pixel of the 8-tall
 *     character/bar cell (i.e., y must be a multiple of CHAR_HEIGHT = 8 for
 *     clean alignment, but any y is valid).
 *
 * Color model:
 *   - set_color(fg, bg) sets the current foreground/background color (RGB565).
 *   - Monochrome displays (e.g. SSD1306) ignore set_color entirely.
 *   - Color displays (e.g. ST7789) apply fg/bg to all subsequent draw calls.
 *   - draw_pixel(on=true)  → fg color;  draw_pixel(on=false) → bg color
 *
 * Primitive layer (pure virtual — every adapter must implement):
 *   init, clear, update, set_color, draw_pixel, width, height
 *
 * Derived layer (virtual with default base implementations — adapters may
 * override for hardware optimisation):
 *   draw_char, draw_string, draw_line, draw_bar
 *
 * Composite layer (virtual with default implementations built on top of the
 * derived layer — adapters may override for hardware acceleration):
 *   draw_status_row, draw_chart
 */
class IDisplay {
public:
    virtual ~IDisplay() = default;

    // -----------------------------------------------------------------------
    // Graph style — used by draw_chart to select the line style.
    // -----------------------------------------------------------------------
    enum GraphStyle : uint8_t {
        GRAPH_STYLE_SOLID  = 0, ///< Continuous filled area
        GRAPH_STYLE_DASHED,     ///< Dashed marker above fill (every 12 samples, 6 on / 6 off)
        GRAPH_STYLE_DOTTED,     ///< Dot marker above fill (every 8th sample)
        GRAPH_STYLE_BOLD,       ///< Extra pixel below the line
    };

    // Character cell size used by the built-in 8×8 font.
    static constexpr uint8_t CHAR_WIDTH  = 8;
    static constexpr uint8_t CHAR_HEIGHT = 8;

    // -----------------------------------------------------------------------
    // Primitive layer — pure virtual; every adapter must implement these.
    // -----------------------------------------------------------------------

    /** Initialize the display hardware. Must be called before any draw call. */
    virtual esp_err_t init() = 0;

    /** Clear the internal framebuffer (does not push to hardware). */
    virtual void clear() = 0;

    /** Flush the framebuffer to the display hardware. */
    virtual esp_err_t update() = 0;

    /**
     * Set the current draw color (RGB565 format).
     * @param fg Foreground color — used when drawing pixels / text.
     * @param bg Background color — used for erase / text background.
     * Monochrome displays may ignore this call.
     */
    virtual void set_color(uint16_t fg, uint16_t bg) = 0;

    /** Set or clear a single pixel in the framebuffer. */
    virtual void draw_pixel(uint8_t x, uint8_t y, bool on) = 0;

    /** Logical display width in pixels. */
    virtual uint16_t width() const = 0;

    /** Logical display height in pixels. */
    virtual uint16_t height() const = 0;

    // -----------------------------------------------------------------------
    // Derived layer — default implementations use draw_pixel.
    // Adapters may override for hardware-level optimisation (e.g. page writes).
    // -----------------------------------------------------------------------

    /**
     * Draw a single character at pixel position (x, y).
     * Default: iterates the 8×8 font bitmap and calls draw_pixel.
     * @param x  Pixel X coordinate (0 to width-1).
     * @param y  Pixel Y coordinate (0 to height-1); top of the character cell.
     * @param c  ASCII character to draw (printable ASCII 0x20–0x7E).
     */
    virtual void draw_char(uint8_t x, uint8_t y, char c);

    /**
     * Draw a null-terminated string starting at pixel position (x, y).
     * Default: calls draw_char for each character, advancing x by CHAR_WIDTH.
     * @param x    Pixel X coordinate.
     * @param y    Pixel Y coordinate; top of the character cell.
     * @param str  Null-terminated ASCII string.
     */
    virtual void draw_string(uint8_t x, uint8_t y, const char *str);

    /**
     * Draw a line between two pixel coordinates using Bresenham's algorithm.
     * Default: calls draw_pixel for each point along the line.
     * @param on  true = draw with fg color, false = draw with bg color.
     */
    virtual void draw_line(int x0, int y0, int x1, int y1, bool on);

    /**
     * Draw a horizontal progress bar (8 pixels tall) at pixel position (y).
     * Default: calls draw_pixel for each bit of the bar pattern.
     * @param y          Pixel Y coordinate; top of the bar cell.
     * @param start_x    Left edge (inclusive).
     * @param end_x      Right edge (inclusive).
     * @param percentage Fill level 0–100.
     */
    virtual void draw_bar(uint8_t y, uint8_t start_x, uint8_t end_x, uint8_t percentage);

    // -----------------------------------------------------------------------
    // Composite layer — high-level drawing helpers built on the derived layer.
    // Adapters may override for hardware acceleration.
    // -----------------------------------------------------------------------

    /**
     * Draw a single-row status display: optional label, value text, and bar.
     *
     * Calls set_color(fg_color, BLACK), then (if label is non-empty)
     * draw_string(label_x, y, label), draw_string(value_x, y, value), and
     * draw_bar(y, bar_start_x, bar_end_x, percent) in that order.
     *
     * @param y          Pixel Y coordinate; top of the row.
     * @param label_x    X position for the label string.
     * @param label      Label text; pass "" to skip.
     * @param value_x    X position for the formatted value string.
     * @param value      Pre-formatted value string (e.g. " 73%").
     * @param bar_start  Left edge of the progress bar (inclusive).
     * @param bar_end    Right edge of the progress bar (inclusive).
     * @param percent    Bar fill level 0–100.
     * @param fg_color   Foreground color (RGB565); also used for text.
     */
    virtual void draw_status_row(uint8_t y,
                                 uint8_t label_x, const char *label,
                                 uint8_t value_x, const char *value,
                                 uint8_t bar_start, uint8_t bar_end,
                                 uint8_t percent, uint16_t fg_color);

    /**
     * Draw a filled area time-series chart.
     *
     * Renders `count` samples (0–100 percentage values) as a filled area
     * chart bounded by (left, top) – (right, bottom). The area below each
     * data point down to `bottom` is filled. The caller is responsible for
     * drawing the axis lines and labels around the chart area.
     *
     * @param left     Left edge of the chart area (x, pixels).
     * @param top      Top edge of the chart area (y, pixels).
     * @param right    Right edge of the chart area (x, pixels).
     * @param bottom   Bottom edge of the chart area (y, pixels).
     * @param samples  Array of `count` percentage values (0–100).
     * @param count    Number of samples; 0 → no-op.
     * @param style    Line/marker style (see GraphStyle).
     * @param fg_color Foreground color (RGB565).
     */
    virtual void draw_chart(uint8_t left, uint8_t top,
                            uint8_t right, uint8_t bottom,
                            const uint8_t *samples, uint8_t count,
                            GraphStyle style, uint16_t fg_color);

private:
    /** Filled trapezoid segment helper used by draw_chart. */
    void _draw_chart_segment(int chart_top, int chart_bottom,
                             int x0, int y0, int x1, int y1);
};

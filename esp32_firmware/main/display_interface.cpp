/**
 * Default implementations for IDisplay derived and composite methods.
 *
 * These use only the primitive layer (draw_pixel, set_color, width, height),
 * so any adapter that implements the 7 pure-virtual primitives gets the full
 * derived and composite layer for free.
 *
 * Adapters may override individual methods for hardware-level optimisation
 * (e.g. SSD1306 overrides draw_char and draw_bar to use page-level writes).
 */

#include "display_interface.hpp"
#include "display_font.hpp"
#include <stdlib.h>  // abs()

// ---------------------------------------------------------------------------
// Derived layer — default implementations
// ---------------------------------------------------------------------------

void IDisplay::draw_char(uint8_t x, uint8_t y, char c)
{
    if (x + CHAR_WIDTH > width() || y + CHAR_HEIGHT > height()) return;

    uint8_t idx = (uint8_t)c;
    if (idx < 32 || idx > 127) idx = 32;
    idx -= 32;

    for (int col = 0; col < CHAR_WIDTH; col++) {
        uint8_t bitmap = display_font::font8x8[idx][col];
        for (int bit = 0; bit < CHAR_HEIGHT; bit++) {
            draw_pixel(x + col, y + bit, (bitmap >> bit) & 1);
        }
    }
}

void IDisplay::draw_string(uint8_t x, uint8_t y, const char *str)
{
    if (!str) return;
    uint8_t cur_x = x;
    const uint16_t disp_w = width();
    while (*str) {
        if (cur_x + CHAR_WIDTH > disp_w) break;
        draw_char(cur_x, y, *str++);
        cur_x += CHAR_WIDTH;
    }
}

void IDisplay::draw_line(int x0, int y0, int x1, int y1, bool on)
{
    const int w = (int)width();
    const int h = (int)height();

    int dx  =  abs(x1 - x0);
    int sx  = (x0 < x1) ? 1 : -1;
    int dy  = -abs(y1 - y0);
    int sy  = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (true) {
        if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) {
            draw_pixel((uint8_t)x0, (uint8_t)y0, on);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void IDisplay::draw_bar(uint8_t y, uint8_t start_x, uint8_t end_x, uint8_t percentage)
{
    if (start_x >= end_x || end_x >= width() || y + CHAR_HEIGHT > height()) return;
    uint8_t w      = end_x - start_x + 1;
    uint8_t filled = (uint8_t)((w * percentage) / 100U);

    for (uint8_t col = 0; col < w; col++) {
        // 0x7E = border/filled (bits 1–6 set); 0x42 = outline (bits 1 and 6 only)
        uint8_t pattern;
        if (col == 0 || col == w - 1) pattern = 0x7E; // border
        else if (col <= filled)       pattern = 0x7E; // filled
        else                          pattern = 0x42; // outline

        for (int bit = 0; bit < CHAR_HEIGHT; bit++) {
            draw_pixel(start_x + col, y + bit, (pattern >> bit) & 1);
        }
    }
}

// ---------------------------------------------------------------------------
// Composite layer
// ---------------------------------------------------------------------------

void IDisplay::draw_status_row(uint8_t y,
                                uint8_t label_x, const char *label,
                                uint8_t value_x, const char *value,
                                uint8_t bar_start, uint8_t bar_end,
                                uint8_t percent, uint16_t fg_color)
{
    set_color(fg_color, DISPLAY_COLOR_BLACK);
    if (label && label[0]) {
        draw_string(label_x, y, label);
    }
    if (value && value[0]) {
        draw_string(value_x, y, value);
    }
    draw_bar(y, bar_start, bar_end, percent);
}

void IDisplay::draw_chart(uint8_t left, uint8_t top,
                          uint8_t right, uint8_t bottom,
                          const uint8_t *samples, uint8_t count,
                          GraphStyle style, uint16_t fg_color)
{
    if (count == 0 || !samples) return;

    set_color(fg_color, DISPLAY_COLOR_BLACK);

    int prev_x = 0, prev_y = 0;

    for (uint8_t i = 0; i < count; ++i) {
        int x = (count > 1U)
            ? left + (int)((i * (right - left)) / (count - 1U))
            : (int)right;
        int y = bottom - (int)((samples[i] * (bottom - top)) / 100U);

        _draw_chart_segment(top, bottom, i > 0 ? prev_x : x, i > 0 ? prev_y : y, x, y);

        if (i > 0) {
            draw_line(prev_x, prev_y, x, y, true);
        }

        // Style markers drawn above/below the data line
        if (style == GRAPH_STYLE_DASHED && (i % 12U) < 6U) {
            draw_pixel((uint8_t)x, (uint8_t)(y > top    ? y - 1 : y), true);
        } else if (style == GRAPH_STYLE_DOTTED && (i % 8U) == 0U) {
            draw_pixel((uint8_t)x, (uint8_t)(y > top    ? y - 1 : y), true);
        } else if (style == GRAPH_STYLE_BOLD) {
            draw_pixel((uint8_t)x, (uint8_t)(y + 1 < bottom ? y + 1 : y), true);
        }

        prev_x = x;
        prev_y = y;
    }
}

// ---------------------------------------------------------------------------
// Private helper
// ---------------------------------------------------------------------------

void IDisplay::_draw_chart_segment(int chart_top, int chart_bottom,
                                   int x0, int y0, int x1, int y1)
{
    if (x0 > x1) {
        int tx = x0, ty = y0;
        x0 = x1; y0 = y1;
        x1 = tx;  y1 = ty;
    }
    const int dx = x1 - x0;
    for (int x = x0; x <= x1; ++x) {
        int y = y0;
        if (dx > 0) y = y0 + ((y1 - y0) * (x - x0)) / dx;
        if (y < chart_top)    y = chart_top;
        if (y > chart_bottom) y = chart_bottom;
        draw_line(x, y, x, chart_bottom, true);
    }
}

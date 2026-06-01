extern "C" {
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

#include "renderer.hpp"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Renderer::Renderer(IDisplay *display)
    : m_display(display)
    , m_last_status{}
    , m_has_status(false)
    , m_now_ms(0)
{
    m_graphs[0] = { "CPU",  IDisplay::GRAPH_STYLE_SOLID,  DISPLAY_COLOR_CYAN,    {}, 0, 0 };
    m_graphs[1] = { "RAM",  IDisplay::GRAPH_STYLE_DASHED, DISPLAY_COLOR_GREEN,   {}, 0, 0 };
    m_graphs[2] = { "GPU",  IDisplay::GRAPH_STYLE_DOTTED, DISPLAY_COLOR_YELLOW,  {}, 0, 0 };
    m_graphs[3] = { "VRAM", IDisplay::GRAPH_STYLE_BOLD,   DISPLAY_COLOR_MAGENTA, {}, 0, 0 };
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Renderer::push_telemetry(const sys_status_t *status)
{
    m_last_status = *status;
    m_has_status  = true;
    graph_push(&m_graphs[0], clamp_percent(status->cpu_usage));
    graph_push(&m_graphs[1], clamp_percent(status->ram_usage));
    graph_push(&m_graphs[2], clamp_percent(status->gpu_usage));
    graph_push(&m_graphs[3], clamp_percent(status->vram_usage));
}

void Renderer::render(display_mode_t mode, bool auto_cycle_active, uint32_t now_ms)
{
    m_now_ms = now_ms;
    if (mode == DISPLAY_MODE_SUMMARY) {
        render_summary_screen();
    } else {
        const resource_graph_t *g = graph_for_mode(mode);
        if (g != nullptr) {
            render_chart_screen(g, auto_cycle_active);
        }
    }
}

void Renderer::render_startup()
{
    m_display->set_color(DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK);
    m_display->clear();
    m_display->draw_string(16, 0 * CHAR_HEIGHT, "PC SYS-MONITOR");
    m_display->draw_string(0,  1 * CHAR_HEIGHT, "----------------");
    m_display->draw_string(12, 3 * CHAR_HEIGHT, "Awaiting Wi-Fi...");
    m_display->draw_string(0,  5 * CHAR_HEIGHT, "Device:");
    m_display->draw_string(16, 6 * CHAR_HEIGHT, "ESP32-SysMon");
}

// ---------------------------------------------------------------------------
// Graph history helpers
// ---------------------------------------------------------------------------

void Renderer::graph_push(resource_graph_t *g, uint8_t value)
{
    g->history[g->head] = value;
    g->head = (g->head + 1U) & HISTORY_MASK;
    if (g->count < HISTORY_LEN) g->count++;
}

uint8_t Renderer::graph_latest(const resource_graph_t *g)
{
    if (g->count == 0) return 0;
    return g->history[(g->head - 1U) & HISTORY_MASK];
}

uint8_t Renderer::graph_sample_at(const resource_graph_t *g, uint8_t offset)
{
    uint8_t oldest = (g->count == HISTORY_LEN) ? g->head : 0;
    return g->history[(oldest + offset) & HISTORY_MASK];
}

const Renderer::resource_graph_t *Renderer::graph_for_mode(display_mode_t mode) const
{
    switch (mode) {
        case DISPLAY_MODE_CPU_CHART:  return &m_graphs[0];
        case DISPLAY_MODE_RAM_CHART:  return &m_graphs[1];
        case DISPLAY_MODE_GPU_CHART:  return &m_graphs[2];
        case DISPLAY_MODE_VRAM_CHART: return &m_graphs[3];
        default:                      return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

uint8_t Renderer::clamp_percent(int value)
{
    if (value < 0)   return 0;
    if (value > 100) return 100;
    return (uint8_t)value;
}

bool Renderer::should_show_label(uint8_t latest) const
{
    if (latest <= ALERT_PERCENT) return true;
    uint32_t phase = m_now_ms / ALERT_BLINK_MS;
    return (phase & 1U) == 0U;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void Renderer::render_summary_row(uint8_t y, const resource_graph_t *g)
{
    char    buf[16];
    uint8_t latest = graph_latest(g);
    uint16_t fg = (latest > ALERT_PERCENT) ? DISPLAY_COLOR_RED : g->color;
    const char *lbl = should_show_label(latest) ? g->label : "";
    snprintf(buf, sizeof(buf), "%3u%%", latest);
    m_display->draw_status_row(y,
                               (uint8_t)GRAPH_LABEL_X, lbl,
                               (uint8_t)GRAPH_VALUE_X, buf,
                               64, 127, latest, fg);
}

void Renderer::render_summary_screen()
{
    char buf[32];

    m_display->clear();
    m_display->set_color(DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK);
    m_display->draw_string(8, 0 * CHAR_HEIGHT, "SYSTEM SUMMARY");

    render_summary_row(1 * CHAR_HEIGHT, &m_graphs[0]); // CPU
    render_summary_row(2 * CHAR_HEIGHT, &m_graphs[1]); // RAM
    render_summary_row(3 * CHAR_HEIGHT, &m_graphs[2]); // GPU
    render_summary_row(4 * CHAR_HEIGHT, &m_graphs[3]); // VRAM

    m_display->set_color(DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK);
    snprintf(buf, sizeof(buf), "WIFI %s", wifi_transport_state_name());
    m_display->draw_string(0, 5 * CHAR_HEIGHT, buf);

    snprintf(buf, sizeof(buf), "IP %s", wifi_transport_sta_ip());
    m_display->draw_string(0, 6 * CHAR_HEIGHT, buf);

    snprintf(buf, sizeof(buf), "UP %luK DN %luK",
             (unsigned long)m_last_status.net_up_kbps,
             (unsigned long)m_last_status.net_down_kbps);
    m_display->draw_string(0, 7 * CHAR_HEIGHT, buf);
}

void Renderer::render_chart_screen(const resource_graph_t *g, bool auto_cycle_active)
{
    char    buf[32];
    uint8_t latest = graph_latest(g);

    m_display->clear();

    uint16_t fg = (latest > ALERT_PERCENT) ? DISPLAY_COLOR_RED : g->color;
    m_display->set_color(fg, DISPLAY_COLOR_BLACK);
    snprintf(buf, sizeof(buf), "%s %3u%% %s",
             should_show_label(latest) ? g->label : "    ",
             latest,
             auto_cycle_active ? "AUTO" : "NEXT");
    m_display->draw_string(0, 0 * CHAR_HEIGHT, buf);

    // Axis labels — gray
    m_display->set_color(DISPLAY_COLOR_GRAY, DISPLAY_COLOR_BLACK);
    m_display->draw_string(0,   1 * CHAR_HEIGHT, "100");
    m_display->draw_string(0,   4 * CHAR_HEIGHT, " 50");
    m_display->draw_string(0,   6 * CHAR_HEIGHT, "  0");
    m_display->draw_string(24,  7 * CHAR_HEIGHT, "-60s");
    m_display->draw_string(104, 7 * CHAR_HEIGHT, "now");

    // Axis lines — gray
    m_display->draw_line(CHART_LEFT, CHART_TOP,    CHART_LEFT,  CHART_BOTTOM, true);
    m_display->draw_line(CHART_LEFT, CHART_BOTTOM, CHART_RIGHT, CHART_BOTTOM, true);
    m_display->draw_line(CHART_LEFT, (CHART_TOP + CHART_BOTTOM) / 2,
                         CHART_RIGHT, (CHART_TOP + CHART_BOTTOM) / 2, true);

    if (g->count == 0) {
        m_display->set_color(DISPLAY_COLOR_WHITE, DISPLAY_COLOR_BLACK);
        m_display->draw_string(32, 3, "Waiting data");
        return;
    }

    uint8_t visible = (g->count < HISTORY_LEN) ? g->count : HISTORY_LEN;
    uint8_t start   = g->count - visible;

    // Collect ordered samples into a contiguous buffer for draw_chart.
    uint8_t sample_buf[HISTORY_LEN];
    for (uint8_t i = 0; i < visible; ++i) {
        sample_buf[i] = graph_sample_at(g, (uint8_t)(start + i));
    }

    m_display->set_color(g->color, DISPLAY_COLOR_BLACK);
    m_display->draw_chart(CHART_LEFT, CHART_TOP, CHART_RIGHT, CHART_BOTTOM,
                          sample_buf, visible, g->style, g->color);
}

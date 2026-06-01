#pragma once

// C/ESP-IDF headers must be wrapped for C++ compilation.
extern "C" {
#include <stdint.h>
#include <stdbool.h>
#include "ble_server.h"
}

#include "display_interface.hpp"

// ---------------------------------------------------------------------------
// Display mode enum — owned here because it drives what the Renderer draws.
// Orchestration code (main.cpp) manages the active mode; Renderer renders it.
// ---------------------------------------------------------------------------
typedef enum {
    DISPLAY_MODE_SUMMARY = 0,
    DISPLAY_MODE_CPU_CHART,
    DISPLAY_MODE_RAM_CHART,
    DISPLAY_MODE_GPU_CHART,
    DISPLAY_MODE_VRAM_CHART,
    DISPLAY_MODE_COUNT,
} display_mode_t;

// ---------------------------------------------------------------------------
// Renderer — converts telemetry state + display mode into draw calls.
//
// Seam: callers inject IDisplay* at construction. Renderer never accesses
// hardware directly, so it can be exercised with any IDisplay adapter
// (real driver or mock) without changing rendering logic.
// ---------------------------------------------------------------------------
class Renderer {
public:
    explicit Renderer(IDisplay *display);

    // Feed a new telemetry sample — advances graph history and caches status.
    void push_telemetry(const sys_status_t *status);

    // Render one frame for the given mode. Calls display->clear() internally;
    // caller must call display->update() after this returns.
    // @param now_ms  Current time in milliseconds (e.g. pdTICKS_TO_MS(xTaskGetTickCount())).
    //                Used for alert blinking; inject from the task to keep Renderer FreeRTOS-free.
    void render(display_mode_t mode, bool auto_cycle_active, uint32_t now_ms);

    // Draw the startup splash (shown before first telemetry arrives).
    // Caller must call display->update() after this returns.
    void render_startup();

private:
    static constexpr uint8_t HISTORY_LEN  = 64;
    static constexpr uint8_t HISTORY_MASK = HISTORY_LEN - 1U;

    // Chart area bounds (pixel coordinates, matching 128×64 canvas)
    static constexpr int GRAPH_LABEL_X   = 0;
    static constexpr int GRAPH_VALUE_X   = 32;
    static constexpr int CHART_LEFT      = 24;
    static constexpr int CHART_TOP       = 14;
    static constexpr int CHART_BOTTOM    = 55;
    static constexpr int CHART_RIGHT     = 127;

    static constexpr uint8_t CHAR_HEIGHT = IDisplay::CHAR_HEIGHT;

    static constexpr uint8_t  ALERT_PERCENT  = 85;
    static constexpr uint32_t ALERT_BLINK_MS = 500;

    struct resource_graph_t {
        const char           *label;
        IDisplay::GraphStyle  style;   // line style passed to draw_chart
        uint16_t              color;
        uint8_t               history[HISTORY_LEN];
        uint8_t               head;
        uint8_t               count;
    };

    IDisplay        *m_display;
    resource_graph_t m_graphs[4];
    sys_status_t     m_last_status;
    bool             m_has_status;
    uint32_t         m_now_ms;  // injected at render() time; used by blink helpers

    // Graph history helpers
    static void    graph_push(resource_graph_t *g, uint8_t value);
    static uint8_t graph_latest(const resource_graph_t *g);
    static uint8_t graph_sample_at(const resource_graph_t *g, uint8_t offset);
    const resource_graph_t *graph_for_mode(display_mode_t mode) const;

    // Rendering helpers
    static uint8_t clamp_percent(int value);
    bool           should_show_label(uint8_t latest) const;

    // Screen renderers
    void render_summary_row(uint8_t y, const resource_graph_t *g);
    void render_summary_screen();
    void render_chart_screen(const resource_graph_t *g, bool auto_cycle_active);
};

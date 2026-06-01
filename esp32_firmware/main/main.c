#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "display_ssd1306.h"
#include "ble_server.h"

static const char *TAG = "Main";

#define HISTORY_LEN 64
#define HISTORY_MASK (HISTORY_LEN - 1U)
#define GRAPH_LABEL_X 0
#define GRAPH_VALUE_X 32
#define GRAPH_PLOT_X 64
#define GRAPH_PLOT_WIDTH 64
#define CHART_LEFT 24
#define CHART_TOP 14
#define CHART_BOTTOM 55
#define CHART_RIGHT 127
#define AUTO_CYCLE_MS 3000

#define BUTTON_PLUS_GPIO GPIO_NUM_0
#define BUTTON_PLUS_ACTIVE_LEVEL 0
#define BUTTON_POLL_MS 25
#define BUTTON_DEBOUNCE_MS 150
#define BUTTON_DOUBLE_CLICK_MS 450

typedef enum {
    GRAPH_STYLE_SOLID = 0,
    GRAPH_STYLE_DASHED,
    GRAPH_STYLE_DOTTED,
    GRAPH_STYLE_BOLD,
} graph_style_t;

typedef struct {
    const char *label;
    graph_style_t style;
    uint8_t history[HISTORY_LEN];
    uint8_t head;
    uint8_t count;
} resource_graph_t;

typedef enum {
    DISPLAY_MODE_SUMMARY = 0,
    DISPLAY_MODE_CPU_CHART,
    DISPLAY_MODE_RAM_CHART,
    DISPLAY_MODE_GPU_CHART,
    DISPLAY_MODE_VRAM_CHART,
    DISPLAY_MODE_COUNT,
} display_mode_t;

static resource_graph_t graphs[] = {
    // Mono OLED fallback: distinct trace styles stand in for color.
    {.label = "CPU",  .style = GRAPH_STYLE_SOLID},
    {.label = "RAM",  .style = GRAPH_STYLE_DASHED},
    {.label = "GPU",  .style = GRAPH_STYLE_DOTTED},
    {.label = "VRAM", .style = GRAPH_STYLE_BOLD},
};

static volatile display_mode_t active_display_mode = DISPLAY_MODE_SUMMARY;
static volatile bool auto_cycle_enabled = false;

static uint8_t clamp_percent(int value)
{
    if (value < 0) return 0;
    if (value > 100) return 100;
    return (uint8_t)value;
}

static void graph_push(resource_graph_t *graph, uint8_t value)
{
    graph->history[graph->head] = value;
    graph->head = (graph->head + 1U) & HISTORY_MASK;
    if (graph->count < HISTORY_LEN) {
        graph->count++;
    }
}

static uint8_t graph_latest(const resource_graph_t *graph)
{
    if (graph->count == 0) return 0;
    uint8_t idx = (graph->head - 1U) & HISTORY_MASK;
    return graph->history[idx];
}

static uint8_t graph_sample_at(const resource_graph_t *graph, uint8_t offset)
{
    uint8_t oldest = (graph->count == HISTORY_LEN) ? graph->head : 0;
    uint8_t idx = (oldest + offset) & HISTORY_MASK;
    return graph->history[idx];
}

static const resource_graph_t *resource_graph_for_mode(display_mode_t mode)
{
    switch (mode) {
        case DISPLAY_MODE_CPU_CHART:
            return &graphs[0];
        case DISPLAY_MODE_RAM_CHART:
            return &graphs[1];
        case DISPLAY_MODE_GPU_CHART:
            return &graphs[2];
        case DISPLAY_MODE_VRAM_CHART:
            return &graphs[3];
        default:
            return NULL;
    }
}

static void advance_display_mode(void)
{
    active_display_mode = (display_mode_t)((active_display_mode + 1) % DISPLAY_MODE_COUNT);
}

static void advance_resource_display_mode(void)
{
    switch (active_display_mode) {
        case DISPLAY_MODE_CPU_CHART:
            active_display_mode = DISPLAY_MODE_RAM_CHART;
            break;
        case DISPLAY_MODE_RAM_CHART:
            active_display_mode = DISPLAY_MODE_GPU_CHART;
            break;
        case DISPLAY_MODE_GPU_CHART:
            active_display_mode = DISPLAY_MODE_VRAM_CHART;
            break;
        default:
            active_display_mode = DISPLAY_MODE_CPU_CHART;
            break;
    }
}

static void toggle_auto_cycle(void)
{
    auto_cycle_enabled = !auto_cycle_enabled;
    if (auto_cycle_enabled && active_display_mode == DISPLAY_MODE_SUMMARY) {
        active_display_mode = DISPLAY_MODE_CPU_CHART;
    }
    ESP_LOGI(TAG, "Auto resource cycle %s", auto_cycle_enabled ? "on" : "off");
}

static void render_summary_row(uint8_t page, const resource_graph_t *graph)
{
    char buffer[16];
    uint8_t latest = graph_latest(graph);

    ssd1306_draw_string(GRAPH_LABEL_X, page, graph->label);
    snprintf(buffer, sizeof(buffer), "%3u%%", latest);
    ssd1306_draw_string(GRAPH_VALUE_X, page, buffer);
    ssd1306_draw_bar(page, 64, 127, latest);
}

static void render_summary_screen(const sys_status_t *status)
{
    char buffer[32];

    ssd1306_clear();
    ssd1306_draw_string(8, 0, "SYSTEM SUMMARY");

    render_summary_row(1, &graphs[0]);
    render_summary_row(2, &graphs[1]);
    render_summary_row(3, &graphs[2]);
    render_summary_row(4, &graphs[3]);

    snprintf(buffer, sizeof(buffer), "WIFI %s", wifi_transport_state_name());
    ssd1306_draw_string(0, 5, buffer);
    snprintf(buffer, sizeof(buffer), "IP %s", wifi_transport_sta_ip());
    ssd1306_draw_string(0, 6, buffer);
    snprintf(buffer, sizeof(buffer), "UP %luK DN %luK",
             (unsigned long)status->net_up_kbps, (unsigned long)status->net_down_kbps);
    ssd1306_draw_string(0, 7, buffer);
}

static void render_chart_screen(const resource_graph_t *graph)
{
    char buffer[32];

    ssd1306_clear();
    snprintf(buffer, sizeof(buffer), "%s %3u%% %s", graph->label, graph_latest(graph),
             auto_cycle_enabled ? "AUTO" : "NEXT");
    ssd1306_draw_string(0, 0, buffer);
    ssd1306_draw_string(0, 1, "100");
    ssd1306_draw_string(0, 4, " 50");
    ssd1306_draw_string(0, 6, "  0");
    ssd1306_draw_string(24, 7, "-60s");
    ssd1306_draw_string(104, 7, "now");

    ssd1306_draw_line(CHART_LEFT, CHART_TOP, CHART_LEFT, CHART_BOTTOM, true);
    ssd1306_draw_line(CHART_LEFT, CHART_BOTTOM, CHART_RIGHT, CHART_BOTTOM, true);
    ssd1306_draw_line(CHART_LEFT, (CHART_TOP + CHART_BOTTOM) / 2,
                      CHART_RIGHT, (CHART_TOP + CHART_BOTTOM) / 2, true);

    if (graph->count == 0) {
        ssd1306_draw_string(32, 3, "Waiting data");
        return;
    }

    uint8_t visible = graph->count < HISTORY_LEN ? graph->count : HISTORY_LEN;
    uint8_t start = graph->count - visible;
    int prev_x = 0;
    int prev_y = 0;

    for (uint8_t i = 0; i < visible; ++i) {
        uint8_t sample = graph_sample_at(graph, (uint8_t)(start + i));
        int x = (visible > 1U)
            ? CHART_LEFT + (int)((i * (CHART_RIGHT - CHART_LEFT)) / (visible - 1U))
            : CHART_RIGHT;
        int y = CHART_BOTTOM - (int)((sample * (CHART_BOTTOM - CHART_TOP)) / 100U);

        if (i > 0) {
            ssd1306_draw_line(prev_x, prev_y, x, y, true);
        } else {
            ssd1306_draw_pixel((uint8_t)x, (uint8_t)y, true);
        }

        if (graph->style == GRAPH_STYLE_DASHED && (i % 12U) < 6U) {
            ssd1306_draw_pixel((uint8_t)x, (uint8_t)(y > CHART_TOP ? y - 1 : y), true);
        } else if (graph->style == GRAPH_STYLE_DOTTED && (i % 8U) == 0U) {
            ssd1306_draw_pixel((uint8_t)x, (uint8_t)(y > CHART_TOP ? y - 1 : y), true);
        } else if (graph->style == GRAPH_STYLE_BOLD) {
            ssd1306_draw_pixel((uint8_t)x, (uint8_t)(y + 1 < CHART_BOTTOM ? y + 1 : y), true);
        }

        prev_x = x;
        prev_y = y;
    }
}

static void button_monitor_task(void *pvParameters)
{
    (void)pvParameters;

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BUTTON_PLUS_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure button GPIO %d: %s", (int)BUTTON_PLUS_GPIO, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    int last_level = gpio_get_level(BUTTON_PLUS_GPIO);
    bool pending_single_click = false;
    TickType_t first_click_tick = 0;
    const TickType_t double_click_ticks = pdMS_TO_TICKS(BUTTON_DOUBLE_CLICK_MS);

    while (1) {
        int level = gpio_get_level(BUTTON_PLUS_GPIO);
        TickType_t now = xTaskGetTickCount();

        if (last_level == 1 && level == BUTTON_PLUS_ACTIVE_LEVEL) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(BUTTON_PLUS_GPIO) == BUTTON_PLUS_ACTIVE_LEVEL) {
                while (gpio_get_level(BUTTON_PLUS_GPIO) == BUTTON_PLUS_ACTIVE_LEVEL) {
                    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
                }

                now = xTaskGetTickCount();
                if (pending_single_click && (now - first_click_tick) <= double_click_ticks) {
                    pending_single_click = false;
                    toggle_auto_cycle();
                } else {
                    pending_single_click = true;
                    first_click_tick = now;
                }
            }
        }

        if (pending_single_click && (now - first_click_tick) > double_click_ticks) {
            pending_single_click = false;
            advance_display_mode();
            ESP_LOGI(TAG, "Display mode -> %d", (int)active_display_mode);
        }

        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

// FreeRTOS Display Updater Task
static void display_updater_task(void *pvParameters)
{
    (void)pvParameters;
    sys_status_t status;
    sys_status_t last_status = {0};
    TickType_t last_auto_cycle_tick = xTaskGetTickCount();

    if (telemetry_queue == NULL) {
        ESP_LOGE(TAG, "Telemetry queue unavailable; display task exiting.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Display task started. Rendering startup screen.");

    // Initial Startup Screen (Awaiting connection)
    ssd1306_clear();
    ssd1306_draw_string(16, 0, "PC SYS-MONITOR");
    ssd1306_draw_string(0, 1, "----------------");
    ssd1306_draw_string(12, 3, "Awaiting Wi-Fi...");
    ssd1306_draw_string(0, 5, "Device:");
    ssd1306_draw_string(16, 6, "ESP32-SysMon");
    ssd1306_update();

    while (1) {
        if (xQueueReceive(telemetry_queue, &status, pdMS_TO_TICKS(1000)) == pdTRUE) {
            last_status = status;
            graph_push(&graphs[0], clamp_percent(status.cpu_usage));
            graph_push(&graphs[1], clamp_percent(status.ram_usage));
            graph_push(&graphs[2], clamp_percent(status.gpu_usage));
            graph_push(&graphs[3], clamp_percent(status.vram_usage));
        }

        TickType_t now = xTaskGetTickCount();
        if (auto_cycle_enabled && (now - last_auto_cycle_tick) >= pdMS_TO_TICKS(AUTO_CYCLE_MS)) {
            advance_resource_display_mode();
            last_auto_cycle_tick = now;
        } else if (!auto_cycle_enabled) {
            last_auto_cycle_tick = now;
        }

        display_mode_t mode = active_display_mode;
        if (mode == DISPLAY_MODE_SUMMARY) {
            render_summary_screen(&last_status);
        } else {
            const resource_graph_t *graph = resource_graph_for_mode(mode);
            if (graph != NULL) {
                render_chart_screen(graph);
            }
        }
        esp_err_t err = ssd1306_update();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SSD1306 update failed: %s", esp_err_to_name(err));
        }
    }
}

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing System Monitor...");

    // 1. Initialize NVS (required for Wi-Fi credential storage)
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. Initialize OLED screen (I2C Driver + SSD1306 configuration)
    bool display_ready = false;
    ret = ssd1306_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 Screen Init Failed! Status code: 0x%02X", ret);
    } else {
        display_ready = true;
    }

    // 3. Initialize Wi-Fi transport and telemetry receiver
    ret = wifi_transport_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi transport init failed! Status code: 0x%02X", ret);
        return;
    }

    // 4. Create UI/Display Task in FreeRTOS
    if (display_ready && xTaskCreate(display_updater_task, "display_updater", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display task.");
        return;
    }

    if (xTaskCreate(button_monitor_task, "button_monitor", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Failed to create button task; chart cycling disabled.");
    }
    
    ESP_LOGI(TAG, "Init complete. System running.");
}

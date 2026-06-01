// C standard / ESP-IDF C headers must be wrapped in extern "C" for C++ compilation.
extern "C" {
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "ble_server.h"
}

#include "display_factory.hpp"
#include "renderer.hpp"

static const char *TAG = "Main";

#define AUTO_CYCLE_MS          3000
#define BUTTON_PLUS_GPIO       GPIO_NUM_0
#define BUTTON_PLUS_ACTIVE     0
#define BUTTON_POLL_MS         25
#define BUTTON_DEBOUNCE_MS     150
#define BUTTON_DOUBLE_CLICK_MS 450

// ---------------------------------------------------------------------------
// Display mode state — managed by button task, consumed by display task.
// Protected by portMUX to ensure atomicity on dual-core ESP32-S3.
// ---------------------------------------------------------------------------
static portMUX_TYPE     g_display_state_mux = portMUX_INITIALIZER_UNLOCKED;
static display_mode_t   active_display_mode = DISPLAY_MODE_SUMMARY;
static bool             auto_cycle_enabled  = false;

// Display and renderer — created once in app_main.
static IDisplay  *display  = nullptr;
static Renderer  *renderer = nullptr;

// ---------------------------------------------------------------------------
// Display mode transitions
// ---------------------------------------------------------------------------
static void advance_display_mode(void)
{
    portENTER_CRITICAL(&g_display_state_mux);
    active_display_mode =
        (display_mode_t)((active_display_mode + 1) % DISPLAY_MODE_COUNT);
    portEXIT_CRITICAL(&g_display_state_mux);
}

static void advance_resource_display_mode(void)
{
    portENTER_CRITICAL(&g_display_state_mux);
    switch (active_display_mode) {
        case DISPLAY_MODE_CPU_CHART:  active_display_mode = DISPLAY_MODE_RAM_CHART;  break;
        case DISPLAY_MODE_RAM_CHART:  active_display_mode = DISPLAY_MODE_GPU_CHART;  break;
        case DISPLAY_MODE_GPU_CHART:  active_display_mode = DISPLAY_MODE_VRAM_CHART; break;
        default:                      active_display_mode = DISPLAY_MODE_CPU_CHART;  break;
    }
    portEXIT_CRITICAL(&g_display_state_mux);
}

static void toggle_auto_cycle(void)
{
    portENTER_CRITICAL(&g_display_state_mux);
    auto_cycle_enabled = !auto_cycle_enabled;
    if (auto_cycle_enabled && active_display_mode == DISPLAY_MODE_SUMMARY) {
        active_display_mode = DISPLAY_MODE_CPU_CHART;
    }
    portEXIT_CRITICAL(&g_display_state_mux);
    ESP_LOGI(TAG, "Auto resource cycle %s", auto_cycle_enabled ? "on" : "off");
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------
static void button_monitor_task(void *pvParameters)
{
    (void)pvParameters;

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask  = 1ULL << BUTTON_PLUS_GPIO;
    io_conf.mode          = GPIO_MODE_INPUT;
    io_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type     = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Button GPIO config failed: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    int last_level = gpio_get_level(BUTTON_PLUS_GPIO);
    bool pending_single_click = false;
    TickType_t first_click_tick = 0;
    const TickType_t double_click_ticks = pdMS_TO_TICKS(BUTTON_DOUBLE_CLICK_MS);

    while (1) {
        int level = gpio_get_level(BUTTON_PLUS_GPIO);
        TickType_t now = xTaskGetTickCount();

        if (last_level == 1 && level == BUTTON_PLUS_ACTIVE) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(BUTTON_PLUS_GPIO) == BUTTON_PLUS_ACTIVE) {
                while (gpio_get_level(BUTTON_PLUS_GPIO) == BUTTON_PLUS_ACTIVE) {
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
            portENTER_CRITICAL(&g_display_state_mux);
            int logged_mode = (int)active_display_mode;
            portEXIT_CRITICAL(&g_display_state_mux);
            ESP_LOGI(TAG, "Display mode -> %d", logged_mode);
        }

        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

static void display_updater_task(void *pvParameters)
{
    (void)pvParameters;
    sys_status_t status;
    TickType_t   last_auto_cycle_tick = xTaskGetTickCount();

    if (telemetry_queue == nullptr) {
        ESP_LOGE(TAG, "Telemetry queue unavailable; display task exiting.");
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Display task started. Rendering startup screen.");
    renderer->render_startup();
    display->update();

    while (1) {
        if (xQueueReceive(telemetry_queue, &status, pdMS_TO_TICKS(1000)) == pdTRUE) {
            renderer->push_telemetry(&status);
        }

        TickType_t now = xTaskGetTickCount();

        portENTER_CRITICAL(&g_display_state_mux);
        bool       cycle_on = auto_cycle_enabled;
        display_mode_t mode = active_display_mode;
        portEXIT_CRITICAL(&g_display_state_mux);

        if (cycle_on && (now - last_auto_cycle_tick) >= pdMS_TO_TICKS(AUTO_CYCLE_MS)) {
            advance_resource_display_mode();
            last_auto_cycle_tick = now;
            portENTER_CRITICAL(&g_display_state_mux);
            mode = active_display_mode;
            portEXIT_CRITICAL(&g_display_state_mux);
        } else if (!cycle_on) {
            last_auto_cycle_tick = now;
        }

        renderer->render(mode, cycle_on,
                         (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()));

        esp_err_t err = display->update();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Display update failed: %s", esp_err_to_name(err));
        }
    }
}

extern "C" void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing System Monitor...");

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize display and renderer
    display = create_display();
    ret = display->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: 0x%02X", ret);
        delete display;
        display = nullptr;
    } else {
        renderer = new Renderer(display);
    }

    ret = wifi_transport_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi transport init failed: 0x%02X", ret);
        return;
    }

    if (display != nullptr && renderer != nullptr &&
        xTaskCreate(display_updater_task, "display_updater", 4096, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display task.");
        return;
    }

    if (xTaskCreate(button_monitor_task, "button_monitor", 4096, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "Failed to create button task; chart cycling disabled.");
    }

    ESP_LOGI(TAG, "Init complete. System running.");
}

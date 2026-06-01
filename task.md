# Tasks: ESP32 PC System Monitor

- [x] Install Python dependencies (`psutil`, `gputil`)
- [x] Install ESP-IDF
- [x] Implement PC service
  - [x] Create `pc_service/requirements.txt`
  - [x] Create `pc_service/monitor_service.py`
- [x] Implement ESP32 firmware
  - [x] Create root `esp32_firmware/CMakeLists.txt`
  - [x] Create `esp32_firmware/sdkconfig.defaults`
  - [x] Create `esp32_firmware/main/CMakeLists.txt`
  - [x] Create `esp32_firmware/main/display_ssd1306.h` and `display_ssd1306.c`
  - [x] Create `esp32_firmware/main/ble_server.h` and `ble_server.c`
  - [x] Create `esp32_firmware/main/main.c`
- [x] Migrate telemetry to Wi-Fi HTTP transport
- [x] Add ESP32 Wi-Fi config/recovery portal
- [x] Add PC LAN discovery frontend
- [x] Add OLED summary dashboard
- [x] Add per-resource chart cycling on + / volume-up
- [x] Improve GPU/VRAM detection fallback
- [x] Verify compilation and code correctness
- [x] Flash updated firmware
- [x] Restart PC frontend service


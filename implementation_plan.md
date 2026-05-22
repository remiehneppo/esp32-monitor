# Current Process: ESP32 PC System Monitor

## Goal
Stream PC telemetry to the ESP32 over **Wi-Fi HTTP**, show a **summary dashboard** on the OLED, and let the **+ / volume-up button** cycle through per-resource charts.

## Current State
- ESP32 firmware is built and flashed.
- PC frontend listens on `0.0.0.0:8080`.
- Default ESP32 station Wi-Fi is `4G CPE E0F8 / 88888888`.
- OLED main screen shows CPU, RAM, GPU, VRAM, Wi-Fi state, IP, and network stats.
- Button cycling is enabled with the default `BUTTON_PLUS_GPIO` set to `GPIO0`.
- PC GPU/VRAM detection now falls back to `nvidia-smi` when `GPUtil` is unavailable.

## Completed Work
1. Replaced BLE telemetry transport with Wi-Fi HTTP transport.
2. Added ESP32 Wi-Fi config portal for SSID/password.
3. Added ESP32 recovery/AP mode and telemetry receiver endpoints.
4. Added PC LAN discovery and device selection frontend.
5. Added OLED summary dashboard.
6. Added per-resource chart views and button-cycled screen switching.
7. Added stronger GPU/VRAM detection on the PC service.
8. Rebuilt and flashed the ESP32 with the current default Wi-Fi credentials.

## Notes
- The summary view is the default screen.
- Pressing the + button cycles: CPU chart -> RAM chart -> GPU chart -> VRAM chart -> summary.
- If the plus/volume-up pin differs on the board, change `BUTTON_PLUS_GPIO` in `esp32_firmware/main/main.c`.


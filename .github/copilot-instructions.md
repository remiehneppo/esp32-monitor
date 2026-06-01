# Copilot instructions for esp32-system-monitor

## Project shape

- `esp32_firmware/` is the ESP-IDF firmware for the ESP32-S3 board.
- `pc_service/` is the host-side Python telemetry sampler and web UI.
- `scripts/` contains setup helpers, especially the systemd installer for the PC service.

## Build, run, and verify

### ESP32 firmware

```bash
cd esp32_firmware
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### PC service

```bash
cd pc_service
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
python monitor_service.py --dry-run
python monitor_service.py --host 0.0.0.0 --port 8080 --allow-manual-targets
```

### Systemd install

```bash
INSTALL_SCOPE=user SERVICE_TARGET=192.168.1.229 scripts/install_pc_service_systemd.sh
SERVICE_TARGET=192.168.1.229 scripts/install_pc_service_systemd.sh
```

### Container path

```bash
docker compose up --build pc-service
```

## High-level architecture

- The PC service samples host CPU, RAM, GPU, VRAM, and network throughput, then streams a packed binary telemetry packet to the ESP32 over HTTP.
- The service also serves a local dashboard and LAN discovery API. It scans the local subnet for ESP32-SysMon devices by calling `/api/info`, then posts telemetry to `/api/telemetry` on the selected board.
- The firmware stores Wi-Fi credentials in NVS, tries STA mode first, and falls back to a recovery AP portal when credentials are missing or retries are exhausted.
- The ESP32 renders a 128x64 SSD1306 OLED UI with a summary screen plus per-resource history charts, and uses the button on GPIO0 to cycle screens / toggle auto-cycling.
- `main.c` owns app startup, display refresh, graph history, and button handling. `ble_server.c` is the Wi-Fi transport/state layer. `recovery_portal.c` serves the config portal and telemetry endpoint. `display_ssd1306.c` owns the OLED driver and drawing primitives.

## Key conventions

- Keep the firmware and Python telemetry format in sync: the packet is packed as `<BBBBII` (`cpu`, `ram`, `gpu`, `vram`, `net_up_kbps`, `net_down_kbps`).
- The PC service only accepts IPv4 targets, with an optional port, through `normalize_target_address()`.
- Manual target selection is gated behind `--allow-manual-targets`; discovery is the default path.
- The service uses a thread-safe `AppState` plus background discovery and telemetry loops; keep state mutations behind that pattern.
- HTTP responses from the PC service are JSON or HTML with `Cache-Control: no-store`, and request logging is intentionally suppressed.
- The OLED is monochrome, so graph styles are used to distinguish CPU, RAM, GPU, and VRAM rather than color.
- `QueueHandle_t telemetry_queue` is a single-slot overwrite queue on the firmware side; telemetry should always replace the previous sample.
- `display_ssd1306.h` hard-codes the board I2C pins and SSD1306 dimensions for the xh-S3E-AI board, so keep display changes consistent with that hardware.

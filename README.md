# ESP32 System Monitor

ESP32-S3 system monitor that receives PC telemetry over Wi-Fi and shows CPU,
RAM, GPU, VRAM, and network activity on an SSD1306 OLED.

## Features

- PC service samples host CPU, RAM, GPU, VRAM, upload, and download usage.
- ESP32 receives telemetry through HTTP on the local network.
- OLED summary screen plus per-resource chart screens.
- Button support for resource screen switching and auto-cycling.
- Wi-Fi recovery portal opens only when credentials are missing or reconnect
  retries are exhausted.

## Repository Layout

- `pc_service/` - Python telemetry sampler and local web dashboard.
- `esp32_firmware/` - ESP-IDF firmware for the ESP32-S3 board.
- `scripts/` - setup scripts for running the PC service with systemd.

## PC Service

Install and run as a user systemd service:

```bash
INSTALL_SCOPE=user SERVICE_TARGET=192.168.1.229 scripts/install_pc_service_systemd.sh
```

Run as a system service with sudo:

```bash
SERVICE_TARGET=192.168.1.229 scripts/install_pc_service_systemd.sh
```

Useful commands:

```bash
systemctl --user status esp32-system-monitor-pc
systemctl --user restart esp32-system-monitor-pc
journalctl --user -u esp32-system-monitor-pc -f
```

Open the dashboard at:

```text
http://127.0.0.1:8080
```

If `SERVICE_TARGET` is set, LAN discovery is disabled by default to reduce CPU
and network overhead. Set `SERVICE_DISCOVERY=1` when installing to keep
automatic discovery enabled.

## Firmware

Build and flash with ESP-IDF:

```bash
cd esp32_firmware
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The firmware exposes:

- `GET /api/info` - device state and Wi-Fi status.
- `POST /api/telemetry` - binary telemetry payload from the PC service.
- Recovery portal endpoints when AP configuration mode is active.

## Notes

- The PC service should run natively on the host for accurate host metrics.
- Docker files are available for quick testing, but Docker may hide GPU access
  unless NVIDIA container support is configured.
- Holding the ESP32 boot button during reset can affect boot mode.

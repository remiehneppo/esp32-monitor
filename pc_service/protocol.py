"""Telemetry wire protocol — single source of truth for firmware ↔ PC format.

Binary packet layout (little-endian):

  Field          | C type   | Fmt | Notes
  ---------------|----------|-----|------------------------
  cpu_usage      | uint8_t  | B   | 0–100 percent
  ram_usage      | uint8_t  | B   | 0–100 percent
  gpu_usage      | uint8_t  | B   | 0–100 percent
  vram_usage     | uint8_t  | B   | 0–100 percent
  net_up_kbps    | uint32_t | I   | kilobytes per second
  net_down_kbps  | uint32_t | I   | kilobytes per second

Firmware side: see esp32_firmware/main/ble_server.h `sys_status_t`.
Adding a field here requires updating sys_status_t, recovery_portal.c JSON,
and the dashboard JS in the same commit.
"""

import struct

TELEMETRY_FORMAT = "<BBBBII"
TELEMETRY_FIELDS = ("cpu", "ram", "gpu", "vram", "net_up_kbps", "net_down_kbps")
TELEMETRY_SIZE   = struct.calcsize(TELEMETRY_FORMAT)

MAX_UINT8  = 0xFF
MAX_UINT32 = 0xFFFFFFFF

DEVICE_NAME = "ESP32-SysMon"


def pack_telemetry(cpu, ram, gpu, vram, net_up_kbps, net_down_kbps) -> bytes:
    """Pack telemetry values into the binary packet matching sys_status_t."""
    return struct.pack(
        TELEMETRY_FORMAT,
        max(0, min(MAX_UINT8,  int(cpu))),
        max(0, min(MAX_UINT8,  int(ram))),
        max(0, min(MAX_UINT8,  int(gpu))),
        max(0, min(MAX_UINT8,  int(vram))),
        max(0, min(MAX_UINT32, int(net_up_kbps))),
        max(0, min(MAX_UINT32, int(net_down_kbps))),
    )

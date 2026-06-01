"""Thread-safe application state shared between discovery, telemetry, and dashboard."""

import threading


def resolve_selected_device(devices, selected_address):
    """Return the device dict matching *selected_address*, or a stub if not found.

    Pure function — no lock, no side effects. Extracted so the resolution logic
    has a testable seam independent of AppState's threading concerns.

    Returns ``None`` if *selected_address* is falsy.
    """
    if not selected_address:
        return None
    device = next((d for d in devices if d["address"] == selected_address), None)
    if device is None:
        device = {"address": selected_address}
    return device


class AppState:
    """Central state container; all mutations are protected by a single lock."""

    def __init__(self):
        self._lock             = threading.Lock()
        self._devices          = []
        self._selected_address = None
        self._discovery_error  = ""
        self._telemetry_error  = ""
        self._latest_stats     = None

    def update_devices(self, devices):
        with self._lock:
            self._devices = devices

    def select(self, address):
        with self._lock:
            self._selected_address = address

    def set_discovery_error(self, message):
        with self._lock:
            self._discovery_error = message

    def set_telemetry_error(self, message):
        with self._lock:
            self._telemetry_error = message

    def set_error(self, message):
        """Backward-compatible alias — sets the telemetry error channel."""
        self.set_telemetry_error(message)

    def update_stats(self, stats):
        with self._lock:
            self._latest_stats = dict(stats)

    def snapshot(self):
        """Return a consistent point-in-time copy of all state."""
        with self._lock:
            devices          = list(self._devices)
            selected         = self._selected_address
            discovery_error  = self._discovery_error
            telemetry_error  = self._telemetry_error
            latest_stats     = dict(self._latest_stats) if self._latest_stats is not None else None

        selected_device = resolve_selected_device(devices, selected)

        return {
            "devices":          devices,
            "selected":         selected,
            "selected_device":  selected_device,
            "discovery_error":  discovery_error,
            "telemetry_error":  telemetry_error,
            # Combined last_error for backward-compat (dashboard shows both)
            "last_error":       telemetry_error or discovery_error,
            "latest_stats":     latest_stats,
        }

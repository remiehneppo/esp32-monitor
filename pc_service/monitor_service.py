#!/usr/bin/env python3
import argparse
import threading
import time
from http.server import ThreadingHTTPServer

from app_state import AppState
from dashboard import AppRequestHandler
from discovery import DISCOVERY_INTERVAL, DeviceScanner, normalize_target_address
from metrics import SystemMonitor
from protocol import pack_telemetry
from telemetry_sender import TelemetrySender

TELEMETRY_INTERVAL = 1.0


def discovery_loop(scanner, state, stop_event):
    while not stop_event.is_set():
        try:
            devices = scanner.discover()
            state.update_devices(devices)
            state.set_discovery_error("")
        except Exception as exc:
            state.set_discovery_error(f"Discovery failed: {exc}")
        stop_event.wait(DISCOVERY_INTERVAL)


def telemetry_loop(monitor, state, stop_event, verbose=False):
    sender = TelemetrySender()
    while not stop_event.is_set():
        cpu, ram, gpu, vram, net_up, net_down = monitor.get_stats()
        state.update_stats({
            "cpu": cpu,
            "ram": ram,
            "gpu": gpu,
            "vram": vram,
            "net_up": net_up,
            "net_down": net_down,
            "updated_at": time.time(),
        })

        snapshot = state.snapshot()
        device = snapshot["selected_device"]
        if device is None:
            stop_event.wait(TELEMETRY_INTERVAL)
            continue

        payload = pack_telemetry(cpu, ram, gpu, vram, net_up, net_down)
        try:
            sender.send(device["address"], payload)
            state.set_telemetry_error("")
            if verbose:
                print(f"Sent to {device['address']}: CPU:{cpu}% RAM:{ram}% GPU:{gpu}% VRAM:{vram}% Up:{net_up}KB/s Down:{net_down}KB/s")
        except ValueError as exc:
            state.set_error(str(exc))
        except Exception as exc:
            state.set_telemetry_error(f"{device['address']}: {exc}")

        stop_event.wait(TELEMETRY_INTERVAL)


def run_dry_run(monitor):
    print("Starting dry run mode (no Wi-Fi transmission). Press Ctrl+C to stop.")
    print("-" * 72)
    print(f"{'Time':<9} | {'CPU%':<5} | {'RAM%':<5} | {'GPU%':<5} | {'VRAM%':<5} | {'NetUp':<10} | {'NetDown':<10}")
    print("-" * 72)
    try:
        while True:
            cpu, ram, gpu, vram, net_up, net_down = monitor.get_stats()
            print(f"{time.strftime('%H:%M:%S'):<9} | {cpu:<5}% | {ram:<5}% | {gpu:<5}% | {vram:<5}% | {net_up:<6} KB/s | {net_down:<6} KB/s")
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\nDry run stopped.")


def main():
    parser = argparse.ArgumentParser(description="PC System Monitor - Wi-Fi streamer")
    parser.add_argument("--dry-run", action="store_true", help="Print stats locally without sending telemetry")
    parser.add_argument("--host", default="127.0.0.1", help="Frontend bind host")
    parser.add_argument("--port", type=int, default=8080, help="Frontend bind port")
    parser.add_argument(
        "--allow-manual-targets",
        action="store_true",
        help="Allow /api/select to target an IP address that was not discovered first",
    )
    parser.add_argument("--target", help="Initial ESP32 target address")
    parser.add_argument("--no-discovery", action="store_true", help="Disable LAN discovery")
    parser.add_argument("--verbose", action="store_true", help="Log each telemetry sample")
    args = parser.parse_args()

    monitor = SystemMonitor()
    if args.dry_run:
        run_dry_run(monitor)
        return

    state = AppState()
    if args.target:
        try:
            state.select(normalize_target_address(args.target))
        except ValueError as exc:
            parser.error(f"--target: {exc}")

    scanner = DeviceScanner()
    stop_event = threading.Event()

    server = ThreadingHTTPServer((args.host, args.port), AppRequestHandler)
    server.app_state = state  # type: ignore[attr-defined]
    server.allow_manual_targets = args.allow_manual_targets  # type: ignore[attr-defined]

    telemetry_thread = threading.Thread(target=telemetry_loop, args=(monitor, state, stop_event, args.verbose), daemon=True)
    if not args.no_discovery:
        discovery_thread = threading.Thread(target=discovery_loop, args=(scanner, state, stop_event), daemon=True)
        discovery_thread.start()
    telemetry_thread.start()

    print(f"Frontend: http://{args.host}:{args.port}")
    if args.no_discovery:
        print("LAN discovery disabled.")
    else:
        print("Scanning LAN for ESP32 SysMon devices...")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        stop_event.set()
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()

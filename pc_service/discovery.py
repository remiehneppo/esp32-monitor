"""Device discovery — scan LAN for ESP32 SysMon boards via /api/info."""

import ipaddress
import json
import logging
import socket
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from urllib.error import URLError
from urllib.request import Request, urlopen

import psutil

from protocol import DEVICE_NAME

DISCOVERY_TIMEOUT  = 0.35
DISCOVERY_INTERVAL = 5.0
SCAN_WORKERS       = 32
MAX_SCAN_HOSTS     = 256


def normalize_target_address(address):
    """Validate and normalise an IPv4 address with optional port.

    Returns ``"host"`` or ``"host:port"`` on success.
    Raises :class:`ValueError` on invalid input.
    """
    address = (address or "").strip()
    if not address:
        raise ValueError("address required")
    if any(ch in address for ch in "/?#@[]\\"):
        raise ValueError("address must be an IPv4 address with optional port")

    host, sep, port = address.partition(":")
    try:
        ipaddress.IPv4Address(host)
    except ipaddress.AddressValueError as exc:
        raise ValueError("address must be an IPv4 address") from exc

    if not sep:
        return host

    try:
        port_num = int(port, 10)
    except ValueError as exc:
        raise ValueError("port must be numeric") from exc
    if port_num < 1 or port_num > 65535:
        raise ValueError("port must be between 1 and 65535")
    return f"{host}:{port_num}"


class DeviceScanner:
    """Scans the local subnet(s) for ESP32 SysMon devices via ``/api/info``."""

    def __init__(self, timeout=DISCOVERY_TIMEOUT):
        self._timeout = timeout

    def discover(self):
        """Return a list of device dicts for every board found on the LAN."""
        hosts = self._candidate_hosts()
        if not hosts:
            return []

        devices = []
        with ThreadPoolExecutor(max_workers=SCAN_WORKERS) as pool:
            futures = [pool.submit(self._probe_host, host) for host in hosts]
            for future in as_completed(futures):
                try:
                    device = future.result()
                except Exception as exc:
                    logging.debug("probe_host future error: %s", exc)
                    continue
                if device:
                    devices.append(device)

        devices.sort(key=lambda d: d["address"])
        return devices

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _candidate_networks(self):
        networks = []
        for iface, addrs in psutil.net_if_addrs().items():
            stats = psutil.net_if_stats().get(iface)
            if stats is None or not stats.isup:
                continue
            for addr in addrs:
                if addr.family != socket.AF_INET:
                    continue
                if addr.address.startswith("127."):
                    continue
                if not addr.netmask:
                    continue
                try:
                    net = ipaddress.IPv4Network(f"{addr.address}/{addr.netmask}", strict=False)
                except ValueError:
                    continue
                if net.num_addresses > MAX_SCAN_HOSTS + 2:
                    try:
                        net = ipaddress.IPv4Network(f"{addr.address}/24", strict=False)
                    except ValueError:
                        continue
                networks.append(net)
        return networks

    def _candidate_hosts(self):
        hosts = []
        for net in self._candidate_networks():
            hosts.extend(str(h) for h in net.hosts())
        return list(dict.fromkeys(hosts))

    def _probe_host(self, host):
        request = Request(f"http://{host}/api/info", headers={"User-Agent": "ESP32-SysMon/1.0"})
        try:
            with urlopen(request, timeout=self._timeout) as response:
                if response.status != 200:
                    return None
                payload = response.read().decode("utf-8", errors="replace")
        except (URLError, TimeoutError, OSError, ValueError):
            return None

        try:
            info = json.loads(payload)
        except json.JSONDecodeError:
            return None

        if info.get("device") != DEVICE_NAME:
            return None

        address = info.get("station_ip") or host
        if not address or address == "0.0.0.0":
            address = host
        try:
            address = normalize_target_address(address)
        except ValueError:
            address = host

        try:
            retry = int(info.get("retry", 0) or 0)
        except (TypeError, ValueError):
            retry = 0

        return {
            "address":    address,
            "host":       host,
            "state":      info.get("state", "unknown"),
            "retry":      retry,
            "telemetry":  bool(info.get("telemetry")),
            "wifi_ssid":  info.get("wifi_ssid", ""),
            "station_ip": info.get("station_ip", ""),
            "ap_ssid":    info.get("ap_ssid", ""),
            "ap_ip":      info.get("ap_ip", ""),
            "last_seen":  time.time(),
        }

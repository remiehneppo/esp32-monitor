#!/usr/bin/env python3
import argparse
import ipaddress
import json
import shutil
import socket
import struct
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit
from urllib.error import URLError
from urllib.request import Request, urlopen

import psutil

try:
    import GPUtil
    HAS_GPUTIL = True
except ImportError:
    HAS_GPUTIL = False

DEVICE_NAME = "ESP32-SysMon"
MAX_UINT32 = (1 << 32) - 1
DISCOVERY_TIMEOUT = 0.35
DISCOVERY_INTERVAL = 5.0
TELEMETRY_INTERVAL = 1.0
SCAN_WORKERS = 32
MAX_SCAN_HOSTS = 256
MAX_JSON_BODY_BYTES = 512


def normalize_target_address(address):
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


class SystemMonitor:
    def __init__(self):
        self.prev_net = psutil.net_io_counters()
        self.prev_time = time.time()

    def _sample_gpu_metrics(self):
        if HAS_GPUTIL:
            try:
                gpus = GPUtil.getGPUs()
                if gpus:
                    g = gpus[0]
                    gpu = int(max(0, min(100, g.load * 100)))
                    vram = 0
                    if g.memoryTotal > 0:
                        vram = int(max(0, min(100, (g.memoryUsed / g.memoryTotal) * 100)))
                    return gpu, vram
            except Exception:
                pass

        if shutil.which("nvidia-smi"):
            try:
                result = subprocess.run(
                    [
                        "nvidia-smi",
                        "--query-gpu=utilization.gpu,memory.used,memory.total",
                        "--format=csv,noheader,nounits",
                    ],
                    capture_output=True,
                    text=True,
                    timeout=1.5,
                    check=False,
                )
            except (OSError, subprocess.TimeoutExpired):
                return 0, 0

            if result.returncode == 0:
                line = next((line.strip() for line in result.stdout.splitlines() if line.strip()), "")
                if line:
                    parts = [part.strip() for part in line.split(",")]
                    if len(parts) >= 3:
                        try:
                            gpu = int(float(parts[0]))
                            vram_used = float(parts[1])
                            vram_total = float(parts[2])
                        except ValueError:
                            return 0, 0
                        vram = int(max(0, min(100, (vram_used / vram_total) * 100))) if vram_total > 0 else 0
                        return int(max(0, min(100, gpu))), vram

        return 0, 0

    def get_stats(self):
        cpu = int(psutil.cpu_percent())
        ram = int(psutil.virtual_memory().percent)
        gpu, vram = self._sample_gpu_metrics()

        now_time = time.time()
        now_net = psutil.net_io_counters()
        dt = now_time - self.prev_time
        if dt <= 0:
            dt = 1.0

        sent_diff = now_net.bytes_sent - self.prev_net.bytes_sent
        recv_diff = now_net.bytes_recv - self.prev_net.bytes_recv
        net_up = int((sent_diff / 1024) / dt)
        net_down = int((recv_diff / 1024) / dt)

        self.prev_net = now_net
        self.prev_time = now_time

        return cpu, ram, gpu, vram, net_up, net_down

    def pack_data(self, cpu, ram, gpu, vram, net_up, net_down):
        net_up = max(0, min(MAX_UINT32, int(net_up)))
        net_down = max(0, min(MAX_UINT32, int(net_down)))
        return struct.pack("<BBBBII", cpu, ram, gpu, vram, net_up, net_down)


class DeviceScanner:
    def __init__(self, timeout=DISCOVERY_TIMEOUT):
        self.timeout = timeout

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
            hosts.extend(str(host) for host in net.hosts())
        return list(dict.fromkeys(hosts))

    def _probe_host(self, host):
        request = Request(f"http://{host}/api/info", headers={"User-Agent": "ESP32-SysMon/1.0"})
        try:
            with urlopen(request, timeout=self.timeout) as response:
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
            "address": address,
            "host": host,
            "state": info.get("state", "unknown"),
            "retry": retry,
            "telemetry": bool(info.get("telemetry")),
            "wifi_ssid": info.get("wifi_ssid", ""),
            "station_ip": info.get("station_ip", ""),
            "ap_ssid": info.get("ap_ssid", ""),
            "ap_ip": info.get("ap_ip", ""),
            "last_seen": time.time(),
        }

    def discover(self):
        hosts = self._candidate_hosts()
        if not hosts:
            return []

        devices = []
        with ThreadPoolExecutor(max_workers=SCAN_WORKERS) as pool:
            futures = [pool.submit(self._probe_host, host) for host in hosts]
            for future in as_completed(futures):
                try:
                    device = future.result()
                except Exception:
                    continue
                if device:
                    devices.append(device)

        devices.sort(key=lambda item: item["address"])
        return devices


class AppState:
    def __init__(self):
        self._lock = threading.Lock()
        self._devices = []
        self._selected_address = None
        self._last_error = ""
        self._latest_stats = None

    def update_devices(self, devices):
        with self._lock:
            self._devices = devices

    def select(self, address, allow_manual=False):
        with self._lock:
            if not allow_manual and not any(device["address"] == address for device in self._devices):
                raise ValueError("address must be one of the discovered ESP32 devices")
            self._selected_address = address

    def set_error(self, message):
        with self._lock:
            self._last_error = message

    def update_stats(self, stats):
        with self._lock:
            self._latest_stats = dict(stats)

    def snapshot(self):
        with self._lock:
            devices = list(self._devices)
            selected = self._selected_address
            last_error = self._last_error
            latest_stats = dict(self._latest_stats) if self._latest_stats is not None else None

        selected_device = None
        for device in devices:
            if device["address"] == selected:
                selected_device = device
                break
        if selected and selected_device is None:
            selected_device = {"address": selected}

        return {
            "devices": devices,
            "selected": selected,
            "selected_device": selected_device,
            "last_error": last_error,
            "latest_stats": latest_stats,
        }


def build_index_html():
    return """<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 SysMon</title>
  <style>
    body{font-family:system-ui,sans-serif;background:#0b1020;color:#e8eefc;margin:0;padding:24px;}
    .card{max-width:960px;margin:0 auto;background:#121a31;border:1px solid #233055;border-radius:16px;padding:24px;}
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px;}
    .device{border:1px solid #2f3d66;border-radius:12px;padding:12px;background:#0a0f1f;}
    .charts{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;margin:16px 0 24px;}
    .metric{border:1px solid #2f3d66;border-left:6px solid var(--metric-color);border-radius:12px;padding:12px;background:#0a0f1f;}
    .metric header{display:flex;align-items:baseline;justify-content:space-between;gap:12px;margin-bottom:8px;}
    .metric strong{color:var(--metric-color);}
    .metric canvas{width:100%;height:72px;display:block;background:#080c18;border-radius:8px;}
    button{background:#4f7cff;color:#fff;border:0;border-radius:10px;padding:10px 14px;font-weight:700;cursor:pointer;}
    .muted{color:#a8b3d6;}
    code{background:#0a0f1f;padding:2px 6px;border-radius:6px;}
    input{width:100%;box-sizing:border-box;padding:10px;border-radius:10px;border:1px solid #334062;background:#0a0f1f;color:#e8eefc;}
    .row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;}
  </style>
</head>
<body>
  <div class="card">
    <h1>ESP32 SysMon devices</h1>
    <p class="muted">Pick an ESP32 on the same Wi-Fi. This service will stream PC telemetry to the selected board over HTTP.</p>
    <p>Status: <code id="status">loading...</code><br>Selected: <code id="selected">none</code><br>Last error: <code id="error">none</code></p>
    <div class="row">
      <input id="manual" placeholder="Optional IP address if it was not auto-discovered">
      <button onclick="selectManual()">Use IP</button>
    </div>
    <div id="charts" class="charts"></div>
    <h2>Discovered devices</h2>
    <div id="devices" class="grid"></div>
  </div>
  <script>
    const metricDefs=[
      {key:'cpu',label:'CPU',color:'#33d17a'},
      {key:'ram',label:'RAM',color:'#62a0ea'},
      {key:'gpu',label:'GPU',color:'#f6d32d'},
      {key:'vram',label:'VRAM',color:'#dc8add'}
    ];
    const metricHistory={cpu:[],ram:[],gpu:[],vram:[]};

    function appendText(parent, tagName, text, className){
      const el=document.createElement(tagName);
      if(className){ el.className=className; }
      el.textContent=text;
      parent.appendChild(el);
      return el;
    }

    function appendBreak(parent){
      parent.appendChild(document.createElement('br'));
    }

    function pushMetricSample(stats){
      if(!stats){ return; }
      for(const metric of metricDefs){
        const value=Number(stats[metric.key]);
        const history=metricHistory[metric.key];
        history.push(Number.isFinite(value) ? Math.max(0,Math.min(100,value)) : 0);
        if(history.length>64){ history.shift(); }
      }
    }

    function drawMetricChart(canvas, metric){
      const history=metricHistory[metric.key];
      const ctx=canvas.getContext('2d');
      const width=canvas.width=canvas.clientWidth*window.devicePixelRatio;
      const height=canvas.height=canvas.clientHeight*window.devicePixelRatio;
      ctx.scale(window.devicePixelRatio,window.devicePixelRatio);
      const w=canvas.clientWidth;
      const h=canvas.clientHeight;
      ctx.clearRect(0,0,w,h);
      ctx.strokeStyle='rgba(255,255,255,0.12)';
      ctx.lineWidth=1;
      for(const y of [0.25,0.5,0.75]){
        ctx.beginPath();
        ctx.moveTo(0,Math.round(h*y)+0.5);
        ctx.lineTo(w,Math.round(h*y)+0.5);
        ctx.stroke();
      }
      if(history.length===0){ return; }
      ctx.strokeStyle=metric.color;
      ctx.lineWidth=3;
      ctx.lineJoin='round';
      ctx.lineCap='round';
      if(history.length===1){
        const y=h-2-(history[0]*(h-4))/100;
        ctx.fillStyle=metric.color;
        ctx.beginPath();
        ctx.arc(w-8,y,4,0,Math.PI*2);
        ctx.fill();
        return;
      }
      ctx.beginPath();
      history.forEach((value,index)=>{
        const x=history.length>1 ? (index*(w-2))/(history.length-1)+1 : w-1;
        const y=h-2-(value*(h-4))/100;
        if(index===0){ ctx.moveTo(x,y); } else { ctx.lineTo(x,y); }
      });
      ctx.stroke();
    }

    function renderCharts(stats){
      pushMetricSample(stats);
      const root=document.getElementById('charts');
      if(!root.children.length){
        for(const metric of metricDefs){
          const card=document.createElement('section');
          card.className='metric';
          card.style.setProperty('--metric-color',metric.color);
          const header=document.createElement('header');
          appendText(header,'strong',metric.label);
          appendText(header,'span','--%','muted').dataset.valueFor=metric.key;
          const canvas=document.createElement('canvas');
          canvas.dataset.chartFor=metric.key;
          card.appendChild(header);
          card.appendChild(canvas);
          root.appendChild(card);
        }
      }
      for(const metric of metricDefs){
        const latest=metricHistory[metric.key].at(-1);
        const valueEl=root.querySelector('[data-value-for="'+metric.key+'"]');
        const canvas=root.querySelector('[data-chart-for="'+metric.key+'"]');
        valueEl.textContent=Number.isFinite(latest) ? Math.round(latest)+'%' : '--%';
        drawMetricChart(canvas,metric);
      }
    }

    async function refresh(){
      const r=await fetch('/api/status',{cache:'no-store'});
      const j=await r.json();
      document.getElementById('status').textContent=j.streaming ? 'streaming' : 'idle';
      document.getElementById('selected').textContent=j.selected || 'none';
      document.getElementById('error').textContent=j.last_error || 'none';
      renderCharts(j.latest_stats);

      const dr=await fetch('/api/devices',{cache:'no-store'});
      const dj=await dr.json();
      const root=document.getElementById('devices');
      root.innerHTML='';
      if(!dj.devices.length){
        appendText(root,'p','No ESP32 SysMon devices found on the LAN yet.','muted');
        return;
      }
      for(const dev of dj.devices){
        const el=document.createElement('div');
        el.className='device';
        appendText(el,'strong',dev.address || '');
        appendBreak(el);
        appendText(el,'span','State: '+(dev.state || '')+' | SSID: '+(dev.wifi_ssid || ''),'muted');
        appendBreak(el);
        appendText(el,'span','AP: '+(dev.ap_ip || '')+' / '+(dev.ap_ssid || ''),'muted');
        appendBreak(el);
        const button=appendText(el,'button','Select');
        button.onclick=()=>selectDevice(dev.address);
        root.appendChild(el);
      }
    }

    async function selectDevice(address){
      await fetch('/api/select',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({address})});
      await refresh();
    }

    async function selectManual(){
      const address=document.getElementById('manual').value.trim();
      if(address){ await selectDevice(address); }
    }

    refresh();
    setInterval(refresh,3000);
  </script>
</body>
</html>"""


class AppRequestHandler(BaseHTTPRequestHandler):
    server_version = "ESP32SysMon/1.0"

    def log_message(self, fmt, *args):
        return

    @property
    def app_state(self):
        return self.server.app_state  # type: ignore[attr-defined]

    @property
    def allow_manual_targets(self):
        return self.server.allow_manual_targets  # type: ignore[attr-defined]

    def _send_json(self, payload, status=200):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, html, status=200):
        body = html.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as exc:
            raise ValueError("invalid content length") from exc
        if length > MAX_JSON_BODY_BYTES:
            raise ValueError("request body too large")
        body = self.rfile.read(length) if length > 0 else b"{}"
        return json.loads(body.decode("utf-8"))

    def do_GET(self):
        if self.path == "/":
            return self._send_html(build_index_html())
        if self.path == "/api/devices":
            snapshot = self.app_state.snapshot()
            return self._send_json({"devices": snapshot["devices"], "selected": snapshot["selected"]})
        if self.path == "/api/status":
            snapshot = self.app_state.snapshot()
            return self._send_json({
                "selected": snapshot["selected"],
                "selected_device": snapshot["selected_device"],
                "last_error": snapshot["last_error"],
                "latest_stats": snapshot["latest_stats"],
                "streaming": snapshot["selected_device"] is not None,
            })
        self.send_error(404, "Not found")

    def do_HEAD(self):
        if self.path == "/":
            body = build_index_html().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return
        self.send_error(404, "Not found")

    def do_POST(self):
        if self.path == "/api/select":
            try:
                payload = self._read_json_body()
            except json.JSONDecodeError:
                return self._send_json({"error": "invalid json"}, status=400)
            except ValueError as exc:
                return self._send_json({"error": str(exc)}, status=400)
            try:
                address = normalize_target_address(payload.get("address"))
            except ValueError as exc:
                return self._send_json({"error": str(exc)}, status=400)
            try:
                self.app_state.select(address, allow_manual=self.allow_manual_targets)
            except ValueError as exc:
                return self._send_json({"error": str(exc)}, status=400)
            self.app_state.set_error("")
            return self._send_json({"ok": True, "selected": address})
        self.send_error(404, "Not found")


def discovery_loop(scanner, state, stop_event):
    while not stop_event.is_set():
        try:
            devices = scanner.discover()
            state.update_devices(devices)
        except Exception as exc:
            state.set_error(f"Discovery failed: {exc}")
        stop_event.wait(DISCOVERY_INTERVAL)


def telemetry_loop(monitor, state, stop_event, verbose=False):
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

        payload = monitor.pack_data(cpu, ram, gpu, vram, net_up, net_down)
        target_url = f"http://{device['address']}/api/telemetry"
        parsed = urlsplit(target_url)
        if parsed.scheme != "http" or parsed.path != "/api/telemetry" or parsed.hostname is None:
            state.set_error(f"{device['address']}: invalid telemetry target")
            stop_event.wait(TELEMETRY_INTERVAL)
            continue
        request = Request(
            target_url,
            data=payload,
            method="POST",
            headers={"Content-Type": "application/octet-stream"},
        )

        try:
            with urlopen(request, timeout=1.0) as response:
                response.read()
            state.set_error("")
            if verbose:
                print(f"Sent to {device['address']}: CPU:{cpu}% RAM:{ram}% GPU:{gpu}% VRAM:{vram}% Up:{net_up}KB/s Down:{net_down}KB/s")
        except Exception as exc:
            state.set_error(f"{device['address']}: {exc}")

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
            state.select(normalize_target_address(args.target), allow_manual=True)
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

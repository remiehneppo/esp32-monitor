"""Local HTTP dashboard — serves the ESP32 SysMon web UI and JSON API."""

import json
from http.server import BaseHTTPRequestHandler

from discovery import normalize_target_address

MAX_JSON_BODY_BYTES = 512


def _build_index_html():
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


_INDEX_HTML = _build_index_html()


def build_index_html():
    return _INDEX_HTML


class AppRequestHandler(BaseHTTPRequestHandler):
    server_version = "ESP32SysMon/1.0"

    def log_message(self, fmt, *args):
        return

    @property
    def _state(self):
        return self.server.app_state  # type: ignore[attr-defined]

    @property
    def _allow_manual(self):
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
            snap = self._state.snapshot()
            return self._send_json({"devices": snap["devices"], "selected": snap["selected"]})
        if self.path == "/api/status":
            snap = self._state.snapshot()
            return self._send_json({
                "selected":         snap["selected"],
                "selected_device":  snap["selected_device"],
                "last_error":       snap["last_error"],
                "telemetry_error":  snap["telemetry_error"],
                "discovery_error":  snap["discovery_error"],
                "latest_stats":     snap["latest_stats"],
                "streaming":        snap["selected_device"] is not None,
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
            snap = self._state.snapshot()
            if not self._allow_manual and not any(
                d["address"] == address for d in snap["devices"]
            ):
                return self._send_json(
                    {"error": "address must be one of the discovered ESP32 devices"},
                    status=400,
                )
            self._state.select(address)
            self._state.set_telemetry_error("")
            return self._send_json({"ok": True, "selected": address})
        self.send_error(404, "Not found")

"""Small HTTP server for the dashboard page (standard library only).

GET  /                 dashboard page
GET  /api/state        latest telemetry, connection, alerts, analysis, recent events
GET  /api/history?since=SEQ           samples after SEQ (incremental polling)
GET  /api/filtered?sensor=0&kind=ma10 raw vs firmware EMA vs a dashboard-side filter
GET  /api/export.csv   this session's CSV log
POST /api/command      JSON {"cmd": "D", "confirm": true}; risky commands need confirm=true
                       {"cmd": "CAL", "values": [d1,d2,d3,w1,w2,w3], "confirm": true}
                       {"cmd": "RAW", "values": [r1,r2,r3], "confirm": true}   {"cmd": "RAW_OFF"}
"""
from __future__ import annotations

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from .station import Station

STATIC = Path(__file__).with_name("static")


def make_server(station: Station, host: str, port: int) -> ThreadingHTTPServer:
    page = (STATIC / "index.html").read_bytes()

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body, ctype="application/json", extra=None):
            if not isinstance(body, (bytes, bytearray)):
                body = json.dumps(body, allow_nan=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(u.query).items()}
            try:
                if u.path in ("/", "/index.html"):
                    self._send(200, page, "text/html; charset=utf-8")
                elif u.path == "/api/state":
                    self._send(200, station.state())
                elif u.path == "/api/history":
                    self._send(200, station.history(int(q.get("since", 0))))
                elif u.path == "/api/filtered":
                    self._send(200, station.filtered(int(q.get("sensor", 0)), q.get("kind", "ma10")))
                elif u.path == "/api/export.csv":
                    if not station.log_path:
                        self._send(404, {"error": "logging is off (--no-log)"})
                        return
                    station._csv_file.flush()
                    self._send(200, Path(station.log_path).read_bytes(), "text/csv",
                               {"Content-Disposition": f'attachment; filename="{Path(station.log_path).name}"'})
                else:
                    self._send(404, {"error": "not found"})
            except ValueError as e:
                self._send(400, {"error": str(e)})

        def do_POST(self):
            if urlparse(self.path).path != "/api/command":
                self._send(404, {"error": "not found"})
                return
            # same-origin only: a page on another site must not be able to command the pump
            origin = self.headers.get("Origin")
            if origin and urlparse(origin).netloc != self.headers.get("Host"):
                self._send(403, {"ok": False, "msg": "cross-origin command refused"})
                return
            try:
                n = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(n) or b"{}")
                ok, msg = station.command(str(body.get("cmd", "")), body.get("confirm") is True, body.get("values"))
            except (ValueError, json.JSONDecodeError) as e:
                ok, msg = False, f"bad request: {e}"
            self._send(200 if ok else 400, {"ok": ok, "msg": msg})

    return ThreadingHTTPServer((host, port), H)

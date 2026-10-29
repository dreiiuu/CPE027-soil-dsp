"""Serve the real dashboard page (extracted from firmware/soil_dsp/dashboard.h) with a mock ESP32 API.

The API replays a logged CSV through the same filters as the firmware (analysis/dsp_ref.py),
one sample per second, so the page can be checked in a browser before flashing.

    python3 tests/dashboard_mock.py --log data/synthetic/synthetic_steady.csv \
        --cal data/synthetic/calibration.json --port 8080
    # then open http://localhost:8080/

Everything shown is a replay of the given log. With the synthetic logs it is SIMULATED data.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "analysis"))
import numpy as np  # noqa: E402
import dsp_ref  # noqa: E402


def load_html() -> str:
    src = (ROOT / "firmware/soil_dsp/dashboard.h").read_text()
    m = re.search(r'R"HTML\((.*)\)HTML"', src, re.S)
    if not m:
        raise SystemExit("dashboard HTML not found in dashboard.h")
    return m.group(1)


def load_log(path: Path):
    rows = []
    for line in path.read_text().splitlines():
        if not line or line.startswith("#") or line.startswith("t_s"):
            continue
        p = line.split(",")
        rows.append([float(p[0]), int(p[1]), int(p[2]), int(p[3]), int(p[4])])
    return np.array(rows)


class Replay:
    def __init__(self, log, cal, theta, start, hist):
        self.log, self.cal, self.start, self.hist = log, cal, start, hist
        a = log[:, 1:4].T
        m = np.vstack([dsp_ref.to_index(a[i], cal["adry"][i], cal["awet"][i]) for i in range(3)])
        x = dsp_ref.median3(m)
        z = dsp_ref.running_median(x, 5)
        self.m, self.x, self.y = m, x, dsp_ref.ema(z, 0.1)
        self.ma = np.convolve(x, np.ones(10) / 10, "full")[: len(x)]
        for i in range(min(9, len(x))):
            self.ma[i] = x[: i + 1].mean()
        self.theta = theta
        self.params = dict(thdry=theta, h=8.0, thwet=80.0, tmax=15, lock=120, blankS=3)
        self.flags = dict(en=0, blank=1, test=0)
        self.t0 = time.time()
        self.lock = threading.Lock()

    def idx(self):
        return min(self.start + int(time.time() - self.t0), len(self.x) - 1)

    def toggles(self, sig, upto):
        s = sig[: upto + 1] >= self.params["thdry"]
        return int(np.count_nonzero(s[1:] != s[:-1]))

    def state(self):
        i = self.idx()
        y = float(self.y[i])
        st = "WET" if y > self.params["thwet"] else ("DRY" if y < self.params["thdry"] else "OK")
        return {
            "t": int(self.log[i, 0]), "a": [int(v) for v in self.log[i, 1:4]],
            "m": [round(float(v), 2) for v in self.m[:, i]],
            "x": round(float(self.x[i]), 2), "y": round(y, 2), "ma": round(float(self.ma[i]), 2),
            "status": st, "pump": int(self.log[i, 4]), "en": self.flags["en"], "test": self.flags["test"],
            "blank": self.flags["blank"], "calok": 1, "cal": {"busy": 0, "type": "dry", "n": 0},
            **self.params,
            "tog": [self.toggles(self.m[0], i), self.toggles(self.x, i), self.toggles(self.ma, i), self.toggles(self.y, i)],
            "ip": "192.168.4.1 (AP) - mock replay",
            "flt": [0, 0, 0], "pins": {"s": [2, 1, 3], "relay": 20, "relayActiveLow": 1},
        }

    def history(self):
        i = self.idx()
        lo = max(0, i - self.hist + 1)
        return {
            "t": [int(v) for v in self.log[lo: i + 1, 0]],
            "a": [[int(v) for v in r] for r in self.log[lo: i + 1, 1:4]],
            "x": [round(float(v), 2) for v in self.x[lo: i + 1]],
            "y": [round(float(v), 2) for v in self.y[lo: i + 1]],
            "ma": [round(float(v), 2) for v in self.ma[lo: i + 1]],
            "p": [int(v) for v in self.log[lo: i + 1, 4]],
        }


def make_handler(html: str, rp: Replay):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, code, body, ctype):
            b = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def do_GET(self):
            u = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(u.query).items()}
            if u.path == "/":
                return self.send(200, html, "text/html")
            if u.path == "/api/state":
                return self.send(200, json.dumps(rp.state()), "application/json")
            if u.path == "/api/history":
                return self.send(200, json.dumps(rp.history()), "application/json")
            if u.path in ("/api/pump", "/api/blank"):
                rp.flags["en" if u.path.endswith("pump") else "blank"] = int(q.get("en", "0") == "1")
                return self.send(200, "ok", "text/plain")
            if u.path == "/api/test":
                rp.flags["test"] = int(q.get("on", "0") == "1")
                return self.send(200, "ok", "text/plain")
            if u.path == "/api/params":
                try:
                    for k_in, k_out in (("thdry", "thdry"), ("h", "h"), ("thwet", "thwet"),
                                        ("tmax", "tmax"), ("lock", "lock"), ("blank", "blankS")):
                        rp.params[k_out] = float(q[k_in])
                except (KeyError, ValueError):
                    return self.send(400, "bad params", "text/plain")
                return self.send(200, "ok", "text/plain")
            if u.path in ("/api/cal", "/api/reset_toggles"):
                return self.send(200, "ok", "text/plain")
            return self.send(404, "not found", "text/plain")

    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=str(ROOT / "data/synthetic/synthetic_steady.csv"))
    ap.add_argument("--cal", default=str(ROOT / "data/synthetic/calibration.json"))
    ap.add_argument("--theta", type=float, default=None, help="dry threshold (default: median of y)")
    ap.add_argument("--start", type=int, default=300, help="sample to start the replay at")
    ap.add_argument("--hist", type=int, default=240, help="history length (firmware keeps 240)")
    ap.add_argument("--port", type=int, default=8080)
    a = ap.parse_args()
    cal = json.loads(Path(a.cal).read_text())
    log = load_log(Path(a.log))
    rp = Replay(log, cal, 0.0, a.start, a.hist)
    theta = a.theta if a.theta is not None else round(float(np.median(rp.y)), 1)
    rp.params["thdry"] = theta
    print(f"mock dashboard on http://localhost:{a.port}/  (replaying {a.log}, theta_dry={theta})", flush=True)
    ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(load_html(), rp)).serve_forever()


if __name__ == "__main__":
    main()

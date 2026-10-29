"""Central state: receives lines from a source, keeps history, logs to CSV, derives alerts.

Thread-safe: the source thread calls on_line/on_status, the HTTP threads call the getters.
"""
from __future__ import annotations

import collections
import csv
import datetime as dt
import threading
import time
from pathlib import Path
from typing import Optional

from . import analysis
from .protocol import COMMANDS, RISKY_COMMANDS, Telemetry, command_line, parse_line

STALE_S = 3.0           # no telemetry for this long while "connected" = no data
HISTORY = 3600          # samples kept in memory (1 h at 1 Hz); the CSV keeps everything

CSV_COLUMNS = ["pc_time", "pc_epoch_s", "source", "ms", "raw1", "raw2", "raw3", "filt1", "filt2", "filt3",
               "pct1", "pct2", "pct3", "average_pct", "calibrated", "sensor_fault", "water_needed",
               "pump_commanded", "led_on", "auto_mode", "lockout", "pulses", "hardware_verified", "paused",
               "test_input", "dry1", "dry2", "dry3", "wet1", "wet2", "wet3", "missing_before"]


class Station:
    def __init__(self, source_kind: str, synthetic: bool, log_dir: Optional[Path],
                 start_pct: int = 50, stop_pct: int = 70, clock=time.time):
        self.clock = clock
        self.lock = threading.Lock()
        self.source_kind, self.synthetic = source_kind, synthetic
        self.start_pct, self.stop_pct = start_pct, stop_pct
        self.conn_state, self.conn_detail, self.port = "connecting", "", ""
        self.latest: Optional[Telemetry] = None
        self.last_rx = None
        self.seq = 0
        self.hist = collections.deque(maxlen=HISTORY)
        self.events = collections.deque(maxlen=300)
        self.stats = collections.Counter()
        self.version_error = ""
        self.last_bad = ""
        self._prev_ms = None
        self._threshold_mismatch_run = 0
        self.threshold_mismatch = False
        self.send_fn = None
        self.log_path = self.event_log_path = None
        self._csv = self._csv_file = self._ev_file = None
        if log_dir is not None:
            self._open_logs(Path(log_dir))

    # ---------------------------------------------------------------- logging
    def _open_logs(self, d: Path):
        d.mkdir(parents=True, exist_ok=True)
        stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        tag = "SYNTHETIC_" if self.synthetic else ""
        self.log_path = d / f"{tag}session_{stamp}.csv"
        self.event_log_path = d / f"{tag}session_{stamp}.events.log"
        self._csv_file = open(self.log_path, "w", newline="", encoding="utf-8")
        if self.synthetic:
            self._csv_file.write(f"# SYNTHETIC DATA from the {self.source_kind}, not a measurement\n")
        self._csv = csv.writer(self._csv_file)
        self._csv.writerow(CSV_COLUMNS)
        self._ev_file = open(self.event_log_path, "a", encoding="utf-8")

    def close(self):
        for f in (self._csv_file, self._ev_file):
            if f:
                f.close()

    def _log_event(self, text: str, origin: str):
        now = self.clock()
        self.events.append({"pc_time": now, "origin": origin, "text": text})
        if self._ev_file:
            self._ev_file.write(f"{_iso(now)}\t{origin}\t{text}\n")
            self._ev_file.flush()

    # ---------------------------------------------------------------- source callbacks
    def on_status(self, state: str, detail: str):
        with self.lock:
            if state != self.conn_state or detail != self.conn_detail:
                self._log_event(f"{state}: {detail}", "app")
            self.conn_state, self.conn_detail = state, detail
            if state == "connected":
                self.port = detail
                self._prev_ms = None          # a reconnect is not a gap in the device's own record

    def on_line(self, line: str):
        p = parse_line(line)
        with self.lock:
            if p.kind == "event":
                self.stats["events"] += 1
                self._log_event(p.text, "device")
            elif p.kind == "noise":
                if p.text:
                    self.stats["noise_lines"] += 1
            elif p.kind == "bad":
                self.stats["bad_lines"] += 1
                self.last_bad = p.error
                self._log_event(f"rejected line: {p.error}", "app")
            elif p.kind == "version":
                self.stats["version_mismatch"] += 1
                if not self.version_error:
                    self._log_event(p.error, "app")
                self.version_error = p.error
            elif p.kind == "telemetry":
                self._add(p.telemetry)

    def _add(self, t: Telemetry):
        now = self.clock()
        missing = 0
        if self._prev_ms is not None:
            d = t.ms - self._prev_ms
            if d < 0:
                self.stats["device_restarts"] += 1
                self._log_event(f"device uptime went back ({self._prev_ms} -> {t.ms} ms): board restarted", "app")
            elif d > 1500:
                missing = int(round(d / 1000.0)) - 1
                self.stats["missing_records"] += missing
                self._log_event(f"{missing} record(s) missing between ms {self._prev_ms} and {t.ms}", "app")
        self._prev_ms = t.ms
        self.version_error = ""
        self.seq += 1
        self.stats["records"] += 1
        self.latest, self.last_rx = t, now
        self.hist.append({"seq": self.seq, "pc": now, "ms": t.ms, "raw": t.raw, "filt": t.filtered_raw,
                          "pct": t.moisture_pct, "avg": t.average_pct, "pump": t.pump_commanded,
                          "demand": t.water_needed, "led": t.led_on, "auto": t.auto_mode, "lock": t.lockout,
                          "fault": t.sensor_fault, "test": bool(t.test_input)})
        self._check_thresholds(t)
        if self._csv:
            b = lambda x: int(bool(x))
            self._csv.writerow([_iso(now), f"{now:.3f}", self.source_kind, t.ms, *t.raw, *t.filtered_raw,
                                *["" if v is None else v for v in t.moisture_pct],
                                "" if t.average_pct is None else t.average_pct,
                                b(t.calibrated), b(t.sensor_fault), b(t.water_needed), b(t.pump_commanded),
                                b(t.led_on), b(t.auto_mode), b(t.lockout), t.pulses, b(t.hardware_verified),
                                "" if t.paused is None else b(t.paused), "" if t.test_input is None else b(t.test_input),
                                *_refs(t.dry_ref), *_refs(t.wet_ref), missing])
            self._csv_file.flush()

    def _check_thresholds(self, t: Telemetry):
        """If the firmware's demand flag contradicts the thresholds shown here for 3 samples in a row,
        the .ino constants and the dashboard disagree. (The firmware latches demand between the two
        thresholds, so only the two outer regions are checked.)"""
        bad = False
        if t.calibrated and not t.sensor_fault and t.average_pct is not None:
            if t.average_pct < self.start_pct and not t.water_needed:
                bad = True
            if t.average_pct >= self.stop_pct and t.water_needed:
                bad = True
        self._threshold_mismatch_run = self._threshold_mismatch_run + 1 if bad else 0
        self.threshold_mismatch = self._threshold_mismatch_run >= 3

    # ---------------------------------------------------------------- commands
    def command(self, cmd: str, confirmed: bool, values=None) -> tuple[bool, str]:
        cmd = cmd.strip()
        sim_only = cmd == "F" and self.source_kind == "simulator"
        if cmd not in COMMANDS and not sim_only:
            return False, f"unknown command {cmd!r}"
        if cmd in RISKY_COMMANDS and not confirmed:
            return False, f"'{cmd}' ({COMMANDS[cmd]}) needs explicit confirmation"
        if self.send_fn is None:
            return False, "no source"
        try:
            text = "F" if sim_only else command_line(cmd, values)
        except (ValueError, TypeError) as e:
            return False, str(e)
        what = COMMANDS.get(cmd, "simulator fault toggle")
        with self.lock:                      # logged before sending so it precedes the board's reply
            self._log_event(f"command {text.strip()} ({what})", "operator")
        try:
            res = self.send_fn(text)
        except Exception as e:  # noqa: BLE001
            with self.lock:
                self._log_event(f"command {cmd} failed: {e}", "app")
            return False, str(e)
        return True, res

    # ---------------------------------------------------------------- views
    def alerts(self) -> list:
        """Ordered most severe first. Each: {level: error|warn|info, code, text}."""
        a = []
        now = self.clock()
        t = self.latest
        if self.synthetic:
            a.append(("info", "synthetic", f"SYNTHETIC data from the {self.source_kind}: not a measurement."))
        if self.conn_state in ("disconnected", "connecting"):
            a.append(("error", "disconnected", f"Board not connected ({self.conn_detail or 'waiting'}). Retrying."))
        elif self.conn_state == "finished":
            a.append(("info", "finished", "Replay finished."))
        elif self.last_rx is None or now - self.last_rx > STALE_S:
            a.append(("error", "no_data", "Connected, but no telemetry is arriving. Wrong port or baud, or the "
                                          "board is running different firmware."))
        if self.version_error:
            a.append(("error", "protocol", f"Telemetry rejected: {self.version_error}. Update this app or the firmware."))
        if t is None:
            return [dict(level=l, code=c, text=x) for l, c, x in a]
        if t.sensor_fault:
            a.append(("error", "sensor_fault", "Sensor fault: a reading is at 0 or 4095 counts. AUTO is off and the pump "
                                               "is held off. Check sensor 3V3, GND and the AO wire."))
        if t.test_input:
            a.append(("warn", "test_input", "TEST INPUT active: the board is using raw values sent with $RAW instead of "
                                            "the sensors, and may pump on them. Ends after 5 min, with End test or X."))
        if t.lockout:
            a.append(("error", "lockout", "Safety lockout: 3 pulses did not bring the soil up to the stop threshold. "
                                          "Inspect the pump, tubing and reservoir, then send X."))
        if not t.calibrated:
            a.append(("warn", "uncalibrated", "Not calibrated: percentages are unavailable. Send D with the probes in dry "
                                              "soil, then W with them in wet soil."))
        if not t.hardware_verified:
            a.append(("warn", "hw_unverified", "Pump wiring not verified: the firmware blocks every pump command "
                                               "(PUMP_HARDWARE_VERIFIED=false)."))
        if self.threshold_mismatch:
            a.append(("warn", "thresholds", f"The firmware's water-demand flag contradicts the displayed thresholds "
                                            f"({self.start_pct}/{self.stop_pct}%). Check the .ino constants."))
        if t.water_needed and t.calibrated:
            a.append(("warn", "water_needed", "Water needed (average below the start threshold)."))
        if t.paused and not t.lockout:
            a.append(("warn", "paused", "AUTO is PAUSED (X, a sensor fault or a dry calibration). It stays paused "
                                        "after a restart. Send A to resume."))
        elif not t.auto_mode and t.calibrated and t.hardware_verified and not t.lockout and not t.sensor_fault:
            a.append(("info", "manual", "AUTO is off: the board will not water by itself."))
        if self.stats["missing_records"]:
            a.append(("info", "missing", f"{self.stats['missing_records']} telemetry record(s) missing this session."))
        return [dict(level=l, code=c, text=x) for l, c, x in a]

    def state(self) -> dict:
        with self.lock:
            t = self.latest
            h = list(self.hist)
            now = self.clock()
            out = {
                "conn": {"state": self.conn_state, "detail": self.conn_detail, "port": self.port,
                         "source": self.source_kind, "synthetic": self.synthetic,
                         "last_rx_age_s": None if self.last_rx is None else round(now - self.last_rx, 1)},
                "t": None if t is None else t.to_dict(),
                "pc_time": None if self.last_rx is None else _iso(self.last_rx),
                "seq": self.seq,
                "stats": dict(self.stats),
                "thresholds": {"start_pct": self.start_pct, "stop_pct": self.stop_pct,
                               "note": "Relative to the two-point dry/wet calibration. Not volumetric water content, "
                                       "not plant-available water."},
                "alerts": self.alerts(),
                "events": list(self.events)[-40:],
                "log_file": str(self.log_path) if self.log_path else None,
                "commands": {k: {"text": v, "risky": k in RISKY_COMMANDS} for k, v in COMMANDS.items()},
            }
        out["analysis"] = self._analysis(h)
        return out

    def _analysis(self, h: list) -> dict:
        if not h:
            return {}
        ts = [r["ms"] / 1000.0 for r in h]
        avg = [r["avg"] for r in h]
        pump = [r["pump"] for r in h]
        rep = analysis.sensor_report([r["raw"] for r in h], [r["pct"] for r in h], ts, pump)
        rate = analysis.drying_rate(ts, avg, pump)
        resp = analysis.pulse_responses(ts, avg, pump)
        return {"sensors": rep, "drying_pct_per_h": None if rate is None else round(rate, 2),
                "pulse_responses": resp[-5:],
                "reference_candidate": analysis.reference_candidate([r["filt"] for r in h if not r.get("test")]),
                "note": "Display-only analysis. The ESP32 controls the pump."}

    def history(self, since: int = 0) -> dict:
        with self.lock:
            rows = [r for r in self.hist if r["seq"] > since]
        keys = ("seq", "pc", "ms", "raw", "filt", "pct", "avg", "pump", "demand", "led", "auto", "lock", "fault")
        return {k: [r[k] for r in rows] for k in keys}

    def filtered(self, sensor: int, kind: str) -> dict:
        if kind not in analysis.FILTERS or sensor not in (0, 1, 2):
            raise ValueError("sensor 0-2 and kind in " + ",".join(analysis.FILTERS))
        with self.lock:
            rows = list(self.hist)
        raw = [r["raw"][sensor] for r in rows]
        name, fn = analysis.FILTERS[kind]
        return {"seq": [r["seq"] for r in rows], "raw": raw, "firmware": [r["filt"][sensor] for r in rows],
                "dashboard": [round(v, 1) for v in fn(raw)], "name": name,
                "spikes": [rows[i]["seq"] for i in analysis.hampel_spikes(raw)]}


def _refs(r):
    return ["" if v is None else v for v in (r or [None] * 3)]


def _iso(epoch: float) -> str:
    return dt.datetime.fromtimestamp(epoch).astimezone().isoformat(timespec="milliseconds")

"""Parser for the ESP32-S3 serial protocol (firmware/smart_irrigation/smart_irrigation.ino, protocol v1).

The firmware prints, at 115200 baud:
  * one JSON object per line, starting with '{', with "type":"telemetry", once per second;
  * human diagnostics starting with '#EVENT '.
Anything else (boot ROM text, partial lines after a reconnect) is counted as noise, never trusted.

This module only parses and validates. It does not decide anything about the pump: water control
stays on the ESP32 (handoff, section 8.8).
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Optional

PROTOCOL_VERSION = 1

BOOL_FIELDS = ("calibrated", "sensor_fault", "water_needed", "pump_commanded", "led_on",
               "auto_mode", "lockout", "hardware_verified")

# Commands the firmware understands. RISKY ones can move water or overwrite calibration,
# so the server refuses them unless the request carries an explicit confirmation.
COMMANDS = {
    "D": "save DRY reference (all three probes dry)",
    "W": "save WET reference (all three probes wet)",
    "A": "enable automatic watering",
    "X": "stop pump, disable AUTO, clear lockout",
    "L": "LED test (2 s)",
    "T": "manual 1.5 s pump pulse",
    "?": "print help",
}
RISKY_COMMANDS = {"D", "W", "A", "T"}


@dataclass
class Telemetry:
    ms: int
    raw: list
    filtered_raw: list
    moisture_pct: list            # int 0-100 or None per sensor
    average_pct: Optional[int]
    calibrated: bool
    sensor_fault: bool
    water_needed: bool
    pump_commanded: bool
    led_on: bool
    auto_mode: bool
    lockout: bool
    pulses: int
    hardware_verified: bool
    v: int = PROTOCOL_VERSION
    extra: dict = field(default_factory=dict)  # unknown keys from a newer firmware, kept for logging

    def to_dict(self) -> dict:
        d = {k: getattr(self, k) for k in ("v", "ms", "raw", "filtered_raw", "moisture_pct", "average_pct",
                                           *BOOL_FIELDS, "pulses")}
        return d


@dataclass
class Parsed:
    kind: str                     # "telemetry" | "event" | "noise" | "bad" | "version"
    telemetry: Optional[Telemetry] = None
    text: str = ""
    error: str = ""


def _int(v, name, lo=None, hi=None):
    if isinstance(v, bool) or not isinstance(v, int):
        raise ValueError(f"{name} must be an integer")
    if (lo is not None and v < lo) or (hi is not None and v > hi):
        raise ValueError(f"{name}={v} out of range")
    return v


def _triple(v, name, lo, hi, nullable=False):
    if not isinstance(v, list) or len(v) != 3:
        raise ValueError(f"{name} must be a list of 3")
    out = []
    for i, x in enumerate(v):
        if x is None and nullable:
            out.append(None)
        else:
            out.append(_int(x, f"{name}[{i}]", lo, hi))
    return out


def parse_line(line: str) -> Parsed:
    line = line.strip()
    if not line:
        return Parsed("noise", text="")
    if line.startswith("#EVENT"):
        return Parsed("event", text=line[len("#EVENT"):].strip())
    if not line.startswith("{"):
        return Parsed("noise", text=line)
    try:
        obj = json.loads(line)
    except json.JSONDecodeError as e:
        return Parsed("bad", text=line, error=f"invalid JSON: {e.msg}")
    if not isinstance(obj, dict) or obj.get("type") != "telemetry":
        return Parsed("noise", text=line)
    if obj.get("v") != PROTOCOL_VERSION:
        return Parsed("version", text=line, error=f"protocol v{obj.get('v')} (this app understands v{PROTOCOL_VERSION})")
    try:
        t = Telemetry(
            ms=_int(obj["ms"], "ms", 0),
            raw=_triple(obj["raw"], "raw", 0, 4095),
            filtered_raw=_triple(obj["filtered_raw"], "filtered_raw", 0, 4095),
            moisture_pct=_triple(obj["moisture_pct"], "moisture_pct", 0, 100, nullable=True),
            average_pct=None if obj["average_pct"] is None else _int(obj["average_pct"], "average_pct", 0, 100),
            pulses=_int(obj["pulses"], "pulses", 0),
            **{k: obj[k] for k in BOOL_FIELDS},
        )
    except KeyError as e:
        return Parsed("bad", text=line, error=f"missing field {e.args[0]}")
    except ValueError as e:
        return Parsed("bad", text=line, error=str(e))
    for k in BOOL_FIELDS:
        if not isinstance(getattr(t, k), bool):
            return Parsed("bad", text=line, error=f"{k} must be true/false")
    known = {"type", "v", "ms", "raw", "filtered_raw", "moisture_pct", "average_pct", "pulses", *BOOL_FIELDS}
    t.extra = {k: v for k, v in obj.items() if k not in known}
    return Parsed("telemetry", telemetry=t, text=line)


def format_telemetry(t: Telemetry) -> str:
    """Byte-for-byte the line the firmware's emitTelemetry() prints (used by the simulator)."""
    def arr(a):
        return "[" + ",".join("null" if x is None else str(x) for x in a) + "]"
    b = lambda x: "true" if x else "false"
    avg = "null" if t.average_pct is None else str(t.average_pct)
    return ('{"type":"telemetry","v":1,"ms":%d,"raw":%s,"filtered_raw":%s,"moisture_pct":%s,"average_pct":%s,'
            '"calibrated":%s,"sensor_fault":%s,"water_needed":%s,"pump_commanded":%s,"led_on":%s,'
            '"auto_mode":%s,"lockout":%s,"pulses":%d,"hardware_verified":%s}') % (
        t.ms, arr(t.raw), arr(t.filtered_raw), arr(t.moisture_pct), avg, b(t.calibrated), b(t.sensor_fault),
        b(t.water_needed), b(t.pump_commanded), b(t.led_on), b(t.auto_mode), b(t.lockout), t.pulses,
        b(t.hardware_verified))

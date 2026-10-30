"""Parser for the ESP32-S3 serial protocol (firmware/smart_irrigation/smart_irrigation.ino, protocol v1).

Firmware v2.0 keeps "v":1 and only adds fields (paused, test_input, dry_ref, wet_ref). They are
optional here, so logs from firmware v1.0 still parse.

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
    "W": "save WET reference (all three probes wet); arms AUTO",
    "A": "resume automatic watering",
    "X": "stop pump, pause AUTO (kept across restarts), clear lockout, end test input",
    "L": "LED test (2 s)",
    "T": "manual 1.5 s pump pulse",
    "?": "print help and the stored calibration",
    "CAL": "set dry/wet raw references from values ($CAL)",
    "RAW": "TEST INPUT: replace the sensor readings with given raw values for 5 min ($RAW)",
    "RAW_OFF": "end the test input ($RAW OFF)",
}
RISKY_COMMANDS = {"D", "W", "A", "T", "CAL", "RAW"}

# limits the firmware applies to $CAL / $RAW (keep in sync with the .ino)
ADC_BAD_LOW, ADC_BAD_HIGH, MIN_CALIBRATION_SPAN_ADC = 10, 4085, 150


def command_line(cmd: str, values=None) -> str:
    """Text to send for a command. Raises ValueError when the firmware would reject it anyway."""
    if cmd in ("D", "W", "A", "X", "L", "T", "?"):
        return cmd
    if cmd == "RAW_OFF":
        return "$RAW OFF\n"
    vals = [int(v) for v in (values or [])]
    if cmd == "CAL":
        if len(vals) != 6:
            raise ValueError("CAL needs 6 values: dry1 dry2 dry3 wet1 wet2 wet3")
        for i in range(3):
            d, w = vals[i], vals[i + 3]
            for v in (d, w):
                if not ADC_BAD_LOW < v < ADC_BAD_HIGH:
                    raise ValueError(f"S{i + 1}: {v} is at the ADC rail (needs {ADC_BAD_LOW + 1}-{ADC_BAD_HIGH - 1})")
            if abs(d - w) < MIN_CALIBRATION_SPAN_ADC:
                raise ValueError(f"S{i + 1}: dry {d} and wet {w} are only {abs(d - w)} counts apart (need {MIN_CALIBRATION_SPAN_ADC})")
        return "$CAL " + " ".join(map(str, vals)) + "\n"
    if cmd == "RAW":
        if len(vals) != 3 or not all(0 <= v <= 4095 for v in vals):
            raise ValueError("RAW needs 3 values 0-4095")
        return "$RAW " + " ".join(map(str, vals)) + "\n"
    raise ValueError(f"unknown command {cmd!r}")


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
    # firmware v2.0 additions (None = not sent, i.e. firmware v1.0)
    paused: Optional[bool] = None
    test_input: Optional[bool] = None
    dry_ref: Optional[list] = None
    wet_ref: Optional[list] = None
    status: Optional[str] = None  # "DRY" / "WET"; not sent while uncalibrated or faulted
    extra: dict = field(default_factory=dict)  # unknown keys from a newer firmware, kept for logging

    def to_dict(self) -> dict:
        d = {k: getattr(self, k) for k in ("v", "ms", "raw", "filtered_raw", "moisture_pct", "average_pct",
                                           *BOOL_FIELDS, "pulses", "paused", "test_input", "dry_ref", "wet_ref", "status")}
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
    try:
        for k in ("paused", "test_input"):
            if k in obj:
                if not isinstance(obj[k], bool):
                    raise ValueError(f"{k} must be true/false")
                setattr(t, k, obj[k])
        for k in ("dry_ref", "wet_ref"):
            if k in obj:
                setattr(t, k, _triple(obj[k], k, 0, 4095, nullable=True))
        if "status" in obj:
            if obj["status"] not in ("DRY", "WET"):
                raise ValueError("status must be DRY or WET")
            t.status = obj["status"]
    except ValueError as e:
        return Parsed("bad", text=line, error=str(e))
    known = {"type", "v", "ms", "raw", "filtered_raw", "moisture_pct", "average_pct", "pulses", *BOOL_FIELDS,
             "paused", "test_input", "dry_ref", "wet_ref", "status"}
    t.extra = {k: v for k, v in obj.items() if k not in known}
    return Parsed("telemetry", telemetry=t, text=line)


def format_telemetry(t: Telemetry) -> str:
    """Byte-for-byte the line the firmware's emitTelemetry() prints (used by the simulator)."""
    def arr(a):
        return "[" + ",".join("null" if x is None else str(x) for x in a) + "]"
    b = lambda x: "true" if x else "false"
    avg = "null" if t.average_pct is None else str(t.average_pct)
    line = ('{"type":"telemetry","v":1,"ms":%d,"raw":%s,"filtered_raw":%s,"moisture_pct":%s,"average_pct":%s,'
            '"calibrated":%s,"sensor_fault":%s,"water_needed":%s,"pump_commanded":%s,"led_on":%s,'
            '"auto_mode":%s,"lockout":%s,"pulses":%d,"hardware_verified":%s}') % (
        t.ms, arr(t.raw), arr(t.filtered_raw), arr(t.moisture_pct), avg, b(t.calibrated), b(t.sensor_fault),
        b(t.water_needed), b(t.pump_commanded), b(t.led_on), b(t.auto_mode), b(t.lockout), t.pulses,
        b(t.hardware_verified))
    if t.paused is None:                       # firmware v1.0 line
        return line
    line = line[:-1] + ',"paused":%s,"test_input":%s,"dry_ref":%s,"wet_ref":%s}' % (
        b(t.paused), b(t.test_input), arr(t.dry_ref), arr(t.wet_ref))
    return line if t.status is None else line[:-1] + ',"status":"%s"}' % t.status

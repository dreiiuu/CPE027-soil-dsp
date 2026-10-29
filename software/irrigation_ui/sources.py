"""Where telemetry lines come from: the real board over USB serial, a simulator, or a replayed log.

Every source hands raw text lines to the same callback, so the simulator and replay exercise exactly
the parser and dashboard code the real board does.
"""
from __future__ import annotations

import random
import threading
import time
from typing import Callable, Optional

from .protocol import Telemetry, format_telemetry

LineFn = Callable[[str], None]
StatusFn = Callable[[str, str], None]   # (state, detail); state in connecting|connected|disconnected|finished

# USB vendor IDs seen on ESP32-S3 dev boards: Espressif native USB, CP210x, CH340/CH343, FTDI
KNOWN_VIDS = {0x303A: "Espressif USB", 0x10C4: "CP210x", 0x1A86: "CH34x", 0x0403: "FTDI"}


class Source:
    kind = "?"
    synthetic = False

    def __init__(self, on_line: LineFn, on_status: StatusFn):
        self.on_line, self.on_status = on_line, on_status
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self.port_name = ""

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True, name=f"source-{self.kind}")
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3)

    def send(self, cmd: str) -> str:
        raise NotImplementedError

    def _run(self):
        raise NotImplementedError


# ------------------------------------------------------------------ real board
def list_ports():
    from serial.tools import list_ports as lp
    return [(p.device, p.description, p.vid) for p in lp.comports()]


def autodetect_port() -> Optional[str]:
    try:
        ports = list_ports()
    except ImportError:
        return None
    cands = [d for d, _, vid in ports if vid in KNOWN_VIDS]
    return cands[0] if len(cands) == 1 else None


class SerialSource(Source):
    kind = "serial"

    def __init__(self, on_line, on_status, port: Optional[str], baud: int = 115200):
        super().__init__(on_line, on_status)
        self.port, self.baud = port, baud
        self._ser = None
        self._lock = threading.Lock()

    def _open(self):
        import serial  # pyserial; imported here so --sim works without it
        port = self.port or autodetect_port()
        if not port:
            raise OSError("no port given and no single ESP32 USB port found (use --port, see --list-ports)")
        s = serial.Serial(port, self.baud, timeout=1, write_timeout=1)
        self.port_name = port
        return s

    def _run(self):
        backoff = 1.0
        while not self._stop.is_set():
            if self._ser is None:
                self.on_status("connecting", self.port or "auto-detect")
                try:
                    with self._lock:
                        self._ser = self._open()
                    self.on_status("connected", self.port_name)
                    backoff = 1.0
                except Exception as e:  # noqa: BLE001 - any open failure means "retry later"
                    self.on_status("disconnected", str(e))
                    self._stop.wait(backoff)
                    backoff = min(backoff * 2, 8.0)
                    continue
            try:
                data = self._ser.readline()
            except Exception as e:  # noqa: BLE001 - unplugged cable, driver error
                self.on_status("disconnected", f"{self.port_name}: {e}")
                with self._lock:
                    try:
                        self._ser.close()
                    except Exception:  # noqa: BLE001
                        pass
                    self._ser = None
                continue
            if data:
                self.on_line(data.decode("utf-8", errors="replace"))
        with self._lock:
            if self._ser is not None:
                self._ser.close()

    def send(self, cmd: str) -> str:
        with self._lock:
            if self._ser is None:
                raise ConnectionError("board not connected")
            self._ser.write(cmd.encode("ascii"))
            self._ser.flush()
        return "sent"


# ------------------------------------------------------------------ simulator
class FirmwareModel:
    """Python port of the control logic in smart_irrigation.ino v2.0, driven by a toy soil model.

    Same constants, same order of operations. tests/test_irrigation_ui.py replays the scenario of
    the real firmware's PC build through it and compares. The soil model is invented: it only
    exists so the dashboard has something plausible to show. All its output is SYNTHETIC.
    """
    START_WATERING_PCT, STOP_WATERING_PCT = 50, 70
    PUMP_PULSE_MS, SOAK_INTERVAL_MS, BOOT_SETTLE_MS, MAX_PULSES = 1500, 20000, 5000, 3
    TEST_INPUT_MAX_MS = 300000
    MIN_CALIBRATION_SPAN_ADC, ADC_BAD_LOW, ADC_BAD_HIGH, EMA_ALPHA = 150, 10, 4085, 0.25
    AUTO_START_AFTER_CALIBRATION = True

    # invented sensor characteristics (counts in air / in water) for the soil model
    AIR = [2950, 3380, 3400]
    WATER = [1480, 1700, 1720]

    def __init__(self, emit: LineFn, hardware_verified=False, calibrated=True, seed=None):
        self.emit = emit
        self.rng = random.Random(seed)
        self.hw = hardware_verified
        self.ms = 0
        self.dry = list(self.AIR) if calibrated else [-1, -1, -1]
        self.wet = list(self.WATER) if calibrated else [-1, -1, -1]
        self.raw, self.filt, self.finit = [0, 0, 0], [0.0, 0.0, 0.0], False
        self.fault = self.auto = self.pump = self.manual = self.demand = self.led = False
        self.paused = self.lockout = False            # "NVS"
        self.led_test, self.led_test_ms = False, 0
        self.test, self.test_raw, self.test_ms = False, [0, 0, 0], 0
        self.pulses, self.pump_start, self.last_stop, self.last_sample, self.boot_ms = 0, 0, 0, 0, 0
        # soil model: "true" relative wetness per probe, water in transit from each pulse
        self.m = [57.0, 55.0, 58.0]
        self.gain = [1.1, 0.9, 1.0]
        self.transit = []          # (arrival_ms, amount)
        self.forced_fault = -1
        self._booted = False

    # --- firmware functions
    def set_pump(self, on):
        if on and not self.hw:
            on = False
        if on != self.pump:
            if on:
                self.pump_start = self.ms
                self.emit("#EVENT pump_command_on")
                self.transit.append((self.ms + 3000, 8.0))     # water reaches probes ~3 s later
            else:
                self.last_stop = self.ms
                self.manual = False
                self.emit("#EVENT pump_command_off")
            self.pump = on

    def set_paused(self, p, why):
        if p != self.paused:
            self.paused = p
            self.emit(("#EVENT auto_paused_" if p else "#EVENT auto_resumed_") + why)

    def _counts(self, i):
        if i == self.forced_fault:
            return 0
        c = self.AIR[i] - self.m[i] / 100.0 * (self.AIR[i] - self.WATER[i]) + self.rng.gauss(0, 4)
        if self.rng.random() < 0.01:
            c += self.rng.choice((-1, 1)) * 160                # occasional glitch
        return max(0, min(4095, int(round(c))))

    def sample(self):
        for i in range(3):
            self.raw[i] = self.test_raw[i] if self.test else self._counts(i)
            self.filt[i] = self.raw[i] if not self.finit else self.filt[i] + self.EMA_ALPHA * (self.raw[i] - self.filt[i])
        self.finit = True
        self.fault = any(r <= self.ADC_BAD_LOW or r >= self.ADC_BAD_HIGH for r in self.raw)

    def cal_valid(self):
        return all(self.dry[i] >= 0 and self.wet[i] >= 0 and abs(self.dry[i] - self.wet[i]) >= self.MIN_CALIBRATION_SPAN_ADC
                   for i in range(3))

    def pct(self, i):
        if not self.cal_valid() or self.fault:
            return -1
        p = 100.0 * (self.filt[i] - self.dry[i]) / float(self.wet[i] - self.dry[i])
        return max(0, min(100, int(_c_lround(p))))

    def avg(self):
        if not self.cal_valid() or self.fault:
            return -1
        return (self.pct(0) + self.pct(1) + self.pct(2)) // 3

    def auto_allowed(self):
        return (self.AUTO_START_AFTER_CALIBRATION and self.hw and self.cal_valid() and not self.fault
                and not self.paused and not self.lockout)

    def print_cal(self):
        self.emit("#EVENT calibration dry=%s wet=%s %s" % (",".join(map(str, self.dry)), ",".join(map(str, self.wet)),
                                                          "valid" if self.cal_valid() else "invalid"))

    def store_cal(self, dry, wet):
        if dry:
            self.dry = list(dry)
        if wet:
            self.wet = list(wet)
        self.pulses = 0
        self.emit("#EVENT calibration_valid" if self.cal_valid() else "#EVENT calibration_incomplete_or_span_too_small")
        self.print_cal()
        if wet and self.cal_valid():
            self.lockout = False
            self.set_paused(False, "after_wet_calibration")
            self.boot_ms = self.ms                    # settleFromMs in the .ino
            self.auto = self.auto_allowed()
            if self.auto:
                self.emit("#EVENT auto_enabled")
        else:
            self.set_paused(True, "for_calibration")

    def capture(self, dry):
        self.auto = self.demand = False
        self.set_pump(False)
        ref = self.AIR if dry else self.WATER     # simulated: as if the probes were really in dry / wet soil
        measured = [int(round(ref[i] + self.rng.gauss(0, 3))) for i in range(3)]
        if not self.test:
            for i in range(3):
                self.raw[i] = measured[i]
                self.filt[i] = float(measured[i])
            self.finit, self.fault = True, False
        self.emit("#EVENT dry_reference_saved" if dry else "#EVENT wet_reference_saved")
        self.store_cal(measured if dry else None, None if dry else measured)

    def control(self):
        if self.fault:
            if self.auto:
                self.emit("#EVENT auto_stopped_sensor_fault")
            self.auto = self.demand = False
            self.set_pump(False)
            if self.cal_valid():
                self.set_paused(True, "sensor_fault")
            return
        if not self.cal_valid():
            self.auto = self.demand = False
            if not self.manual:
                self.set_pump(False)
            return
        p = self.avg()
        if p < self.START_WATERING_PCT:
            self.demand = True
        if p >= self.STOP_WATERING_PCT:
            self.demand = False
            self.pulses = 0
            if not self.manual:
                self.set_pump(False)
        if not self.auto and self.auto_allowed():
            self.auto = True
            self.emit("#EVENT auto_enabled")
        if not self.auto or self.lockout or not self.demand or self.pump:
            return
        if self.ms - self.boot_ms < self.BOOT_SETTLE_MS:
            return
        if not (self.pulses == 0 or self.ms - self.last_stop >= self.SOAK_INTERVAL_MS):
            return
        if self.pulses >= self.MAX_PULSES:
            self.lockout, self.auto = True, False
            self.set_pump(False)
            self.emit("#EVENT safety_lockout_max_pulses")
            return
        self.set_pump(True)
        if self.pump:
            self.pulses += 1

    def update_led(self):
        self.led = self.led_test or (self.fault and (self.ms // 400) % 2 == 0) or (self.cal_valid() and self.demand)

    def telemetry(self) -> Telemetry:
        pcts = [self.pct(i) for i in range(3)]
        a = self.avg()
        ref = lambda r: [None if v < 0 else v for v in r]
        return Telemetry(ms=self.ms, raw=list(self.raw), filtered_raw=[int(_c_lround(f)) for f in self.filt],
                         moisture_pct=[None if p < 0 else p for p in pcts], average_pct=None if a < 0 else a,
                         calibrated=self.cal_valid(), sensor_fault=self.fault, water_needed=self.demand,
                         pump_commanded=self.pump, led_on=self.led, auto_mode=self.auto, lockout=self.lockout,
                         pulses=self.pulses, hardware_verified=self.hw, paused=self.paused, test_input=self.test,
                         dry_ref=ref(self.dry), wet_ref=ref(self.wet))

    def end_test(self, why):
        if self.test:
            self.test, self.finit = False, False
            self.emit("#EVENT test_input_off_" + why)

    def line(self, text):
        """A $-command, text without the '$' and the newline (as handleLine() receives it)."""
        text = text.upper()
        parts = text.split()
        if text.startswith("CAL "):
            try:
                v = [int(x) for x in parts[1:]]
                assert len(v) == 6 and all(0 <= x <= 4095 for x in v)
            except (ValueError, AssertionError):
                self.emit("#EVENT cal_rejected_need_6_values_0_4095")
                return
            for i in range(3):
                if not all(self.ADC_BAD_LOW < x < self.ADC_BAD_HIGH for x in (v[i], v[i + 3])) or \
                        abs(v[i] - v[i + 3]) < self.MIN_CALIBRATION_SPAN_ADC:
                    self.emit("#EVENT cal_rejected_rail_value_or_span_below_150")
                    return
            self.auto = self.demand = False
            self.set_pump(False)
            self.emit("#EVENT calibration_set_from_values")
            self.store_cal(v[:3], v[3:])
        elif text == "RAW OFF":
            self.end_test("by_command")
        elif text.startswith("RAW "):
            try:
                v = [int(x) for x in parts[1:]]
                assert len(v) == 3 and all(0 <= x <= 4095 for x in v)
            except (ValueError, AssertionError):
                self.emit("#EVENT raw_rejected_need_3_values_0_4095")
                return
            self.test_raw = v
            if not self.test:
                self.finit = False
            self.test, self.test_ms = True, self.ms
            self.emit("#EVENT test_input_on")
        else:
            self.emit("#EVENT unknown_line_command")

    def command(self, c):
        if c.startswith("$"):
            self.line(c[1:].strip())
            return
        c = c.upper()
        if c == "D":
            self.capture(True)
        elif c == "W":
            self.capture(False)
        elif c == "L":
            self.led_test, self.led_test_ms = True, self.ms
            self.emit("#EVENT led_test_2s")
        elif c == "X":
            self.set_pump(False)
            self.auto = self.lockout = self.demand = False
            self.set_paused(True, "by_X")
            self.pulses = 0
            self.end_test("by_X")
            self.emit("#EVENT stopped_and_lockout_cleared")
        elif c == "A":
            if not self.hw:
                self.emit("#EVENT auto_denied_check_NO_COM_and_trigger_polarity")
            elif self.lockout:
                self.emit("#EVENT auto_denied_lockout_send_X_then_review")
            elif not self.cal_valid():
                self.emit("#EVENT auto_denied_calibrate_D_then_W")
            elif self.fault:
                self.emit("#EVENT auto_denied_sensor_fault")
            elif self.pump:
                self.emit("#EVENT auto_denied_pump_busy")
            else:
                self.set_paused(False, "by_A")
                self.auto, self.demand, self.pulses = True, False, 0
                self.emit("#EVENT auto_enabled")
        elif c == "T":
            if not self.hw or self.fault or self.auto or self.lockout or self.pump:
                self.emit("#EVENT manual_pump_test_denied")
            else:
                self.manual = True
                self.set_pump(True)
                self.emit("#EVENT manual_pump_test_1p5s")
        elif c == "?":
            self.emit("#EVENT commands D=dry W=wet A=auto X=stop/pause L=LED T=pump ?=help | "
                      "$CAL d1 d2 d3 w1 w2 w3 | $RAW r1 r2 r3 | $RAW OFF")
            self.print_cal()

    # --- time
    def _soil(self, dt_s):
        for i in range(3):
            self.m[i] = max(0.0, self.m[i] - 0.10 * dt_s)       # invented drying rate, fast enough to watch
        keep = []
        for arrive, amount in self.transit:
            if self.ms >= arrive:
                for i in range(3):
                    self.m[i] = min(100.0, self.m[i] + amount * self.gain[i])
            else:
                keep.append((arrive, amount))
        self.transit = keep

    def boot(self):
        self.emit("#EVENT SYNTHETIC_SIMULATOR (not a real board)")
        self.boot_ms = self.ms
        self.sample()
        self.auto = self.auto_allowed()
        self.emit("#EVENT esp32_s3_irrigation_v2_boot")
        self.emit("#EVENT led_startup_test_complete")
        if not self.hw:
            self.emit("#EVENT PUMP_DISABLED_UNTIL_HARDWARE_VERIFIED")
        if not self.cal_valid():
            self.emit("#EVENT calibration_needed_send_D_and_W")
        if self.paused:
            self.emit("#EVENT auto_paused_send_A_to_resume")
        if self.auto:
            self.emit("#EVENT auto_enabled_at_boot")
        self.last_sample = self.ms
        self._booted = True

    def advance(self, step_ms=100):
        if not self._booted:
            self.boot()
        self.ms += step_ms
        self._soil(step_ms / 1000.0)
        if self.pump and self.ms - self.pump_start >= self.PUMP_PULSE_MS:
            self.set_pump(False)
        if self.led_test and self.ms - self.led_test_ms >= 2000:
            self.led_test = False
        if self.test and self.ms - self.test_ms >= self.TEST_INPUT_MAX_MS:
            self.end_test("timeout")
        if self.ms - self.last_sample >= 1000:
            self.last_sample = self.ms
            self.sample()
            self.control()
            self.update_led()
            self.emit(format_telemetry(self.telemetry()))
        else:
            self.update_led()


def _c_lround(x):
    """C lroundf: halves away from zero (Python round() is banker's rounding)."""
    import math
    return int(math.floor(x + 0.5)) if x >= 0 else -int(math.floor(-x + 0.5))


class SimSource(Source):
    kind = "simulator"
    synthetic = True

    def __init__(self, on_line, on_status, speed=1.0, hardware_verified=False, calibrated=True, seed=None):
        super().__init__(on_line, on_status)
        self.speed = speed
        self.model = FirmwareModel(on_line, hardware_verified=hardware_verified, calibrated=calibrated, seed=seed)
        self._lock = threading.Lock()
        self.port_name = "simulator"

    def _run(self):
        self.on_status("connected", "SYNTHETIC simulator")
        step = 100
        while not self._stop.is_set():
            with self._lock:
                self.model.advance(step)
            self._stop.wait(step / 1000.0 / self.speed)

    def send(self, cmd: str) -> str:
        with self._lock:
            cmd = cmd.strip()
            if cmd == "F":                       # simulator only: toggle a disconnected-probe fault on S3
                self.model.forced_fault = -1 if self.model.forced_fault == 2 else 2
                return "simulated S3 fault " + ("on" if self.model.forced_fault == 2 else "off")
            self.model.command(cmd)
        return "sent"


# ------------------------------------------------------------------ replay
class ReplaySource(Source):
    kind = "replay"

    def __init__(self, on_line, on_status, path, speed=1.0, loop=False, synthetic=True):
        super().__init__(on_line, on_status)
        self.path, self.speed, self.loop, self.synthetic = path, speed, loop, synthetic
        self.port_name = str(path)

    def _run(self):
        self.on_status("connected", f"replay of {self.path}")
        while not self._stop.is_set():
            with open(self.path, encoding="utf-8") as f:
                for line in f:
                    if self._stop.is_set():
                        return
                    self.on_line(line)
                    if line.lstrip().startswith("{"):
                        self._stop.wait(1.0 / self.speed)
            if not self.loop:
                self.on_status("finished", f"end of {self.path}")
                return

    def send(self, cmd: str) -> str:
        raise ConnectionError("commands are disabled while replaying a log")

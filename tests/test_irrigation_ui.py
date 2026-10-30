"""Tests for the PC dashboard (software/irrigation_ui). Standard library only.

  python3 tests/test_irrigation_ui.py [path/to/fw_serial.txt]

The optional file is the serial output of the REAL firmware run on the PC (tests/fw_host_run.cpp,
`make app-test` produces it). With it, the firmware -> app link is checked end to end.
"""
from __future__ import annotations

import json
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "software"))

from irrigation_ui import analysis  # noqa: E402
from irrigation_ui.protocol import command_line, format_telemetry, parse_line  # noqa: E402
from irrigation_ui.server import make_server  # noqa: E402
from irrigation_ui.sources import FirmwareModel, SimSource  # noqa: E402
from irrigation_ui.station import Station  # noqa: E402

FW_SERIAL = None
FW_SERIAL_VERIFIED = None
EXAMPLE = ('{"type":"telemetry","v":1,"ms":60123,"raw":[2580,3160,3197],"filtered_raw":[2582,3163,3196],'
           '"moisture_pct":[44,48,52],"average_pct":48,"calibrated":true,"sensor_fault":false,"water_needed":true,'
           '"pump_commanded":false,"led_on":true,"auto_mode":true,"lockout":false,"pulses":1,"hardware_verified":true}')


class FakeClock:
    def __init__(self):
        self.t = 1_700_000_000.0

    def __call__(self):
        return self.t


def feed(station, lines, clock=None):
    for ln in lines:
        if clock and ln.startswith("{"):
            clock.t += 1.0
        station.on_line(ln)


class Protocol(unittest.TestCase):
    def test_handoff_example(self):
        p = parse_line(EXAMPLE)
        self.assertEqual(p.kind, "telemetry")
        t = p.telemetry
        self.assertEqual((t.ms, t.raw, t.moisture_pct, t.average_pct, t.pulses), (60123, [2580, 3160, 3197], [44, 48, 52], 48, 1))
        self.assertTrue(t.water_needed and t.led_on and t.auto_mode and t.hardware_verified)
        self.assertEqual(format_telemetry(t), EXAMPLE)     # simulator prints byte-identical lines

    def test_nulls(self):
        s = EXAMPLE.replace('[44,48,52]', '[null,null,null]').replace('"average_pct":48', '"average_pct":null')
        t = parse_line(s).telemetry
        self.assertEqual((t.moisture_pct, t.average_pct), ([None, None, None], None))
        self.assertEqual(format_telemetry(t), s)

    def test_events_and_noise(self):
        self.assertEqual(parse_line("#EVENT pump_command_on\r\n").kind, "event")
        self.assertEqual(parse_line("#EVENT pump_command_on").text, "pump_command_on")
        for s in ("ESP-ROM:esp32s3-20210327", "", "rst:0x1 (POWERON),boot:0x8", '{"type":"other"}'):
            self.assertEqual(parse_line(s).kind, "noise", s)

    def test_v2_fields(self):
        s = EXAMPLE[:-1] + ',"paused":true,"test_input":false,"dry_ref":[2950,3380,3400],"wet_ref":[null,null,null]}'
        t = parse_line(s).telemetry
        self.assertEqual((t.paused, t.test_input, t.dry_ref, t.wet_ref), (True, False, [2950, 3380, 3400], [None] * 3))
        self.assertEqual(format_telemetry(t), s)
        self.assertIsNone(parse_line(EXAMPLE).telemetry.paused)           # v1.0 firmware line still fine
        self.assertEqual(parse_line(s.replace('"paused":true', '"paused":1')).kind, "bad")
        self.assertEqual(parse_line(s.replace('[2950,3380,3400]', '[2950,3380]')).kind, "bad")

    def test_command_lines(self):
        self.assertEqual(command_line("CAL", [3200, 3300, 3350, 2000, 2100, 2150]), "$CAL 3200 3300 3350 2000 2100 2150\n")
        self.assertEqual(command_line("RAW", [2800, 3000, 3100]), "$RAW 2800 3000 3100\n")
        self.assertEqual(command_line("RAW_OFF"), "$RAW OFF\n")
        self.assertEqual(command_line("A"), "A")
        for bad in ([3200, 3300, 3350, 3100, 2100, 2150],      # S1 span 100 < 150
                    [4090, 3300, 3350, 2000, 2100, 2150],      # rail
                    [3200, 3300, 3350, 2000, 2100]):           # 5 values
            with self.assertRaises(ValueError):
                command_line("CAL", bad)
        with self.assertRaises(ValueError):
            command_line("RAW", [1, 2, 5000])

    def test_rejects(self):
        self.assertEqual(parse_line(EXAMPLE.replace('"v":1', '"v":2')).kind, "version")
        self.assertEqual(parse_line(EXAMPLE[:-20]).kind, "bad")                                   # cut-off line
        self.assertEqual(parse_line(EXAMPLE.replace('"pulses":1,', '')).kind, "bad")                # missing field
        self.assertEqual(parse_line(EXAMPLE.replace('[2580,3160,3197]', '[2580,3160]')).kind, "bad")
        self.assertEqual(parse_line(EXAMPLE.replace('[44,48,52]', '[44,48,152]')).kind, "bad")     # out of range
        self.assertEqual(parse_line(EXAMPLE.replace('"lockout":false', '"lockout":0')).kind, "bad")
        self.assertEqual(parse_line(EXAMPLE.replace('"pulses":1', '"pulses":true')).kind, "bad")


@unittest.skipUnless(FW_SERIAL or len(sys.argv) > 1, "needs the firmware host-run output (make app-test)")
class FirmwareToApp(unittest.TestCase):
    """The real .ino's serial output, through the app."""

    @classmethod
    def setUpClass(cls):
        cls.lines = Path(FW_SERIAL).read_text().splitlines()
        cls.parsed = [parse_line(l) for l in cls.lines]
        cls.tel = [p.telemetry for p in cls.parsed if p.kind == "telemetry"]

    def test_every_line_understood(self):
        kinds = {p.kind for p in self.parsed}
        self.assertLessEqual(kinds, {"telemetry", "event"}, [p.error for p in self.parsed if p.kind not in ("telemetry", "event")])
        self.assertGreater(len(self.tel), 40)
        for l in self.lines:
            p = parse_line(l)
            if p.kind == "telemetry":
                self.assertEqual(format_telemetry(p.telemetry), l)    # app's formatter == firmware's emitter

    def test_scenario_states(self):
        t = self.tel
        self.assertEqual(t[0].raw, [2580, 3166, 3197])
        self.assertFalse(t[0].calibrated)
        self.assertEqual(t[0].moisture_pct, [None, None, None])
        self.assertTrue(any(x.calibrated and x.average_pct is not None for x in t))
        self.assertTrue(any(x.water_needed and x.led_on for x in t))
        self.assertTrue(any(x.sensor_fault and x.moisture_pct == [None] * 3 for x in t))
        self.assertFalse(any(x.pump_commanded for x in t))            # pump hardware not verified
        self.assertFalse(any(x.hardware_verified for x in t))
        self.assertFalse(any(x.auto_mode for x in t))
        self.assertTrue(any(x.test_input and x.raw == [2800, 3200, 3220] for x in t))
        self.assertTrue(t[-1].calibrated and t[-1].paused)            # both survived the reboot via NVS
        self.assertEqual(t[-1].dry_ref, [2950, 3380, 3400])
        self.assertEqual(t[-1].wet_ref, [1480, 1700, 1720])

    def test_station(self):
        clk = FakeClock()
        st = Station("serial", False, None, clock=clk)
        st.on_status("connected", "COM7")
        feed(st, self.lines, clk)
        s = st.state()
        self.assertEqual(s["stats"]["device_restarts"], 1)
        self.assertEqual(s["stats"].get("missing_records", 0), 0)
        self.assertEqual(s["stats"].get("bad_lines", 0), 0)
        evs = [e["text"] for e in s["events"]]
        self.assertIn("auto_denied_check_NO_COM_and_trigger_polarity", evs)
        codes = {a["code"] for a in s["alerts"]}
        self.assertIn("hw_unverified", codes)
        self.assertNotIn("thresholds", codes)                         # firmware agrees with the 50/70 display


@unittest.skipUnless(FW_SERIAL_VERIFIED or len(sys.argv) > 2, "needs the verified-build output (make app-test)")
class VerifiedFirmwareToApp(unittest.TestCase):
    """The firmware compiled with PUMP_HARDWARE_VERIFIED=true (automatic watering really runs)."""

    @classmethod
    def setUpClass(cls):
        cls.lines = Path(FW_SERIAL_VERIFIED).read_text().splitlines()
        cls.tel = [parse_line(l).telemetry for l in cls.lines if l.startswith("{")]

    def test_parse_and_states(self):
        for l in self.lines:
            p = parse_line(l)
            self.assertIn(p.kind, ("telemetry", "event"), l)
            if p.kind == "telemetry":
                self.assertEqual(format_telemetry(p.telemetry), l)
        t = self.tel
        self.assertTrue(any(x.pump_commanded for x in t))
        self.assertTrue(any(x.lockout for x in t))
        self.assertTrue(any(x.auto_mode for x in t))
        self.assertTrue(any(x.paused for x in t))
        self.assertTrue(any(x.test_input and x.pump_commanded for x in t))
        self.assertLessEqual(max(x.pulses for x in t), 3)

    def test_station(self):
        clk = FakeClock()
        st = Station("serial", False, None, clock=clk)
        st.on_status("connected", "COM7")
        feed(st, self.lines, clk)
        s = st.state()
        self.assertEqual(s["stats"]["device_restarts"], 3)
        self.assertEqual(s["stats"].get("bad_lines", 0), 0)
        self.assertEqual(s["stats"].get("missing_records", 0), 0)
        evs = [e["text"] for e in st.events]
        for e in ("safety_lockout_max_pulses", "auto_enabled_at_boot", "test_input_on", "auto_paused_sensor_fault"):
            self.assertIn(e, evs)
        self.assertIsNotNone(s["analysis"]["pulse_responses"])


class StationBehaviour(unittest.TestCase):
    def test_gaps_restarts_logging(self):
        import tempfile
        clk = FakeClock()
        with tempfile.TemporaryDirectory() as d:
            st = Station("replay", True, Path(d), clock=clk)
            st.on_status("connected", "x")
            lines = (ROOT / "data/synthetic/Smart_Irrigation_Synthetic_Telemetry.jsonl").read_text().splitlines()
            feed(st, lines, clk)
            s = st.state()
            self.assertEqual(s["stats"]["records"], 6)
            self.assertEqual(s["stats"]["missing_records"], 7 + 1 + 21)   # ms 2000->10000, 11000->13000, 13000->35000
            st.close()
            csv_text = Path(st.log_path).read_text()
            self.assertTrue(csv_text.startswith("# SYNTHETIC"))
            self.assertIn("SYNTHETIC_", Path(st.log_path).name)
            self.assertEqual(len(csv_text.strip().splitlines()), 1 + 1 + 6)

    def test_analysis_export(self):
        import tempfile
        sys.path.insert(0, str(ROOT / "software"))
        import to_analysis_csv
        clk = FakeClock()
        with tempfile.TemporaryDirectory() as d:
            st = Station("serial", False, Path(d), clock=clk)
            st.on_status("connected", "COM7")
            for i in range(5):
                st.on_line(EXAMPLE.replace("60123", str(60123 + 1000 * i)))
            st.close()
            out = Path(d) / "a.csv"
            self.assertEqual(to_analysis_csv.convert(str(st.log_path), str(out)), 5)
            self.assertEqual(out.read_text().splitlines()[:2], ["t_s,s1,s2,s3,pump", "0,2580,3160,3197,0"])

    def test_stale_and_disconnect(self):
        clk = FakeClock()
        st = Station("serial", False, None, clock=clk)
        st.on_status("disconnected", "could not open COM7")
        self.assertEqual(st.alerts()[0]["code"], "disconnected")
        st.on_status("connected", "COM7")
        self.assertEqual(st.alerts()[0]["code"], "no_data")
        st.on_line(EXAMPLE)
        self.assertNotIn("no_data", [a["code"] for a in st.alerts()])
        clk.t += 10
        self.assertIn("no_data", [a["code"] for a in st.alerts()])

    def test_threshold_mismatch(self):
        st = Station("serial", False, None, start_pct=50, stop_pct=70)
        st.on_status("connected", "x")
        bad = EXAMPLE.replace('"water_needed":true', '"water_needed":false')   # avg 48 < 50 but no demand
        for i in range(3):
            st.on_line(bad.replace("60123", str(60123 + 1000 * i)))
        self.assertIn("thresholds", [a["code"] for a in st.alerts()])

    def test_command_rules(self):
        st = Station("serial", False, None)
        sent = []
        st.send_fn = lambda c: sent.append(c) or "sent"
        self.assertFalse(st.command("A", False)[0])
        self.assertFalse(st.command("T", False)[0])
        self.assertFalse(st.command("D", False)[0])
        self.assertTrue(st.command("X", False)[0])          # stopping never needs confirmation
        self.assertTrue(st.command("A", True)[0])
        self.assertFalse(st.command("Z", True)[0])
        self.assertFalse(st.command("F", True)[0])          # simulator-only command on a real board
        self.assertFalse(st.command("CAL", False, [3200, 3300, 3350, 2000, 2100, 2150])[0])
        self.assertFalse(st.command("CAL", True, [3200, 3300, 3350, 3150, 2100, 2150])[0])   # span too small
        self.assertTrue(st.command("CAL", True, [3200, 3300, 3350, 2000, 2100, 2150])[0])
        self.assertFalse(st.command("RAW", False, [2800, 3000, 3100])[0])
        self.assertTrue(st.command("RAW_OFF", False)[0])
        self.assertEqual(sent, ["X", "A", "$CAL 3200 3300 3350 2000 2100 2150\n", "$RAW OFF\n"])


class Simulator(unittest.TestCase):
    def test_unverified_never_pumps(self):
        lines = []
        m = FirmwareModel(lines.append, hardware_verified=False, seed=2)
        for _ in range(50):
            m.advance(100)
        m.command("A")
        m.command("T")
        for _ in range(3000):
            m.advance(100)
        tel = [parse_line(l).telemetry for l in lines if l.startswith("{")]
        self.assertFalse(any(t.pump_commanded for t in tel))
        self.assertIn("#EVENT auto_denied_check_NO_COM_and_trigger_polarity", lines)

    def test_auto_cycle_respects_firmware_limits(self):
        lines = []
        m = FirmwareModel(lines.append, hardware_verified=True, seed=3)
        for _ in range(20):
            m.advance(100)
        m.command("A")
        on_at, pulses_ms, stops = None, [], []
        for _ in range(6000):                               # 10 simulated minutes
            was = m.pump
            m.advance(100)
            if m.pump and not was:
                on_at = m.ms
                if stops:
                    self.assertGreaterEqual(m.ms - stops[-1], m.SOAK_INTERVAL_MS)
            if was and not m.pump:
                pulses_ms.append(m.ms - on_at)
                stops.append(m.ms)
            self.assertLessEqual(m.pulses, m.MAX_PULSES)
        self.assertGreater(len(pulses_ms), 0)
        self.assertTrue(all(p <= m.PUMP_PULSE_MS + 100 for p in pulses_ms), pulses_ms)
        for l in lines:
            self.assertIn(parse_line(l).kind, ("telemetry", "event"), l)

    def test_lockout_when_water_does_not_arrive(self):
        lines = []
        m = FirmwareModel(lines.append, hardware_verified=True, seed=4)
        m.gain = [0.0, 0.0, 0.0]                            # e.g. empty reservoir: probes never get wetter
        m.command("A")
        for _ in range(3000):
            m.advance(100)
        self.assertTrue(m.lockout and not m.auto and not m.pump)
        self.assertIn("#EVENT safety_lockout_max_pulses", lines)


    def test_cal_and_raw_commands(self):
        lines = []
        m = FirmwareModel(lines.append, hardware_verified=True, calibrated=False, seed=5)
        for _ in range(30):
            m.advance(100)
        self.assertFalse(m.auto)
        m.command("$CAL 2950 3380 3400 1480 1700 1720\n")
        self.assertTrue(m.cal_valid() and m.auto)
        m.command("$RAW %d %d %d\n" % tuple(analysis.raw_for_percent(45, d, w) for d, w in zip(m.dry, m.wet)))
        for _ in range(100):
            m.advance(100)
        t = parse_line([l for l in lines if l.startswith("{")][-1]).telemetry
        self.assertTrue(t.test_input and t.water_needed)
        self.assertEqual(t.average_pct, 45)
        self.assertGreaterEqual(m.pulses, 1)
        m.command("X")
        self.assertTrue(m.paused and not m.test and not m.auto)


class Analysis(unittest.TestCase):
    def test_filters(self):
        x = [0.0] * 5 + [10.0] * 20
        self.assertAlmostEqual(analysis.moving_average(x, 10)[-1], 10.0)
        self.assertAlmostEqual(analysis.ema([4.0, 8.0], 0.25)[1], 5.0)
        self.assertEqual(analysis.running_median([1, 100, 1, 1, 1], 5)[2], 1)
        # firmware filter: the spike is removed by the 3-point median, the step is averaged over 5
        y = analysis.median_then_ma([10, 10, 10, 90, 10, 10, 10], 3, 5)
        self.assertEqual(y[3], 10)
        self.assertEqual(analysis.median_then_ma([0] * 5 + [10] * 6, 3, 5)[-1], 10)

    def test_sim_filter_matches_analysis(self):
        lines = []
        m = FirmwareModel(lines.append, seed=3)
        raw = [2000, 2004, 2160, 1998, 2001, 2003, 1840, 2002, 2500, 2500, 2500, 2501]
        for got, want in zip([m._smooth(0, x) for x in raw], analysis.median_then_ma(raw, 3, 5)):
            self.assertAlmostEqual(got, want, places=3)       # float32 in the firmware model
        for _ in range(600):
            m.advance(100)
        tel = [parse_line(l).telemetry for l in lines if l.startswith("{")]
        self.assertTrue(all(t.status in ("DRY", "WET") for t in tel))

    def test_noise_ignores_steps(self):
        import random
        r = random.Random(0)
        x = [2000 + r.gauss(0, 5) for _ in range(300)] + [2300 + r.gauss(0, 5) for _ in range(300)]
        self.assertLess(abs(analysis.noise_sd(x) - 5), 1)          # plain SD would be ~150

    def test_hampel(self):
        x = [2000 + (i % 3) for i in range(60)]
        x[30] = 2200
        self.assertEqual(analysis.hampel_spikes(x), [30])

    def test_drying_and_response(self):
        t = list(range(0, 600))
        y = [60 - 0.01 * s for s in t]                       # 36 %/h drying
        self.assertAlmostEqual(analysis.drying_rate(t, y, [False] * 600), -36.0, places=3)
        pump = [False] * 600
        pump[300] = pump[301] = True
        y2 = [50] * 305 + [53] * 295
        r = analysis.pulse_responses(t, y2, pump)
        self.assertEqual(len(r), 1)
        self.assertEqual(r[0]["lag_s"], 5)

    def test_reference_candidate_and_percent(self):
        steady = [[3200 + (i % 3), 3300, 3350] for i in range(30)]
        c = analysis.reference_candidate(steady)
        self.assertTrue(c["stable"])
        self.assertEqual(c["value"], [3201, 3300, 3350])
        moving = [[3200 + 5 * i, 3300, 3350] for i in range(30)]
        self.assertFalse(analysis.reference_candidate(moving)["stable"])
        # the handoff's illustration: dry 3200, wet 2000
        self.assertEqual([analysis.percent(r, 3200, 2000) for r in (3200, 3000, 2600, 2200, 2000, 3500, 1800)],
                         [0, 17, 50, 83, 100, 0, 100])
        self.assertEqual(analysis.raw_for_percent(50, 3200, 2000), 2600)
        self.assertIsNone(analysis.percent(2500, 3200, 3100))

    def test_disagreement_warning(self):
        raw = [[2500, 2600, 2700]] * 40
        pct = [[20, 50, 80]] * 40
        rep = analysis.sensor_report(raw, pct, list(range(40)), [False] * 40)
        self.assertTrue(any("disagree" in w for w in rep["warnings"]))


class Server(unittest.TestCase):
    def test_http_api(self):
        st = Station("simulator", True, None)
        src = SimSource(st.on_line, st.on_status, speed=20.0, hardware_verified=True)
        st.send_fn = src.send
        httpd = make_server(st, "127.0.0.1", 0)
        port = httpd.server_address[1]
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        src.start()
        base = f"http://127.0.0.1:{port}"
        try:
            deadline = time.time() + 5
            while st.seq < 5 and time.time() < deadline:
                time.sleep(0.05)
            s = json.load(urllib.request.urlopen(base + "/api/state"))
            self.assertTrue(s["conn"]["synthetic"])
            self.assertEqual(len(s["t"]["raw"]), 3)
            h = json.load(urllib.request.urlopen(base + "/api/history?since=0"))
            self.assertGreaterEqual(len(h["seq"]), 5)
            f = json.load(urllib.request.urlopen(base + "/api/filtered?sensor=1&kind=med5ema"))
            self.assertEqual(len(f["raw"]), len(f["dashboard"]))
            self.assertIn(b"Smart Irrigation Monitor", urllib.request.urlopen(base + "/").read())

            def post(body, origin=None):
                req = urllib.request.Request(base + "/api/command", json.dumps(body).encode(),
                                             {"Content-Type": "application/json", **({"Origin": origin} if origin else {})})
                try:
                    return urllib.request.urlopen(req).status
                except urllib.error.HTTPError as e:
                    return e.code
            self.assertEqual(post({"cmd": "T"}), 400)                       # risky, unconfirmed
            self.assertEqual(post({"cmd": "T", "confirm": True}), 200)
            self.assertEqual(post({"cmd": "X"}, origin="http://evil.example"), 403)
            self.assertEqual(post({"cmd": "X"}), 200)
        finally:
            src.stop()
            httpd.shutdown()
            httpd.server_close()


if __name__ == "__main__":
    if len(sys.argv) > 2:
        FW_SERIAL_VERIFIED = sys.argv.pop(2)
    if len(sys.argv) > 1:
        FW_SERIAL = sys.argv.pop(1)
    unittest.main(verbosity=1)

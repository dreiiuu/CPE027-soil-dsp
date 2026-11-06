"""Smart irrigation dashboard (PC side).

  python -m irrigation_ui --port COM7            real board (Windows); /dev/ttyACM0 or /dev/ttyUSB0 on Linux
  python -m irrigation_ui                        real board, auto-detect the ESP32 USB port
  python -m irrigation_ui --sim                  SYNTHETIC simulator, no board needed
  python -m irrigation_ui --replay FILE.jsonl    replay a captured serial log
  python -m irrigation_ui --list-ports

Then open http://127.0.0.1:8765/ . Run from the `software/` folder (or set PYTHONPATH to it).
"""
from __future__ import annotations

import argparse
import sys
import webbrowser
from pathlib import Path

from .server import make_server
from .sources import ReplaySource, SerialSource, SimSource, list_ports
from .station import Station


def main(argv=None):
    ap = argparse.ArgumentParser(prog="irrigation_ui", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--port", help="serial port (COM7, /dev/ttyACM0). Default: auto-detect")
    src.add_argument("--sim", action="store_true", help="SYNTHETIC simulator of the firmware")
    src.add_argument("--replay", type=Path, help="replay a JSONL / serial capture file")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--list-ports", action="store_true")
    ap.add_argument("--http-port", type=int, default=8765)
    ap.add_argument("--host", default="127.0.0.1",
                    help="interface for the dashboard. Keep 127.0.0.1 unless you trust everyone on the network: "
                         "the page can send pump commands")
    ap.add_argument("--log-dir", type=Path, default=Path("logs"))
    ap.add_argument("--no-log", action="store_true")
    ap.add_argument("--start-pct", type=int, default=50, help="displayed start threshold; must match START_WATERING_PCT in the .ino")
    ap.add_argument("--stop-pct", type=int, default=70, help="displayed stop threshold; must match STOP_WATERING_PCT in the .ino")
    ap.add_argument("--speed", type=float, default=1.0, help="simulator / replay speed factor")
    ap.add_argument("--loop", action="store_true", help="replay: start again at the end")
    ap.add_argument("--sim-verified", action="store_true",
                    help="simulator: behave as if PUMP_HARDWARE_VERIFIED=true (shows AUTO watering)")
    ap.add_argument("--sim-fresh", action="store_true", help="simulator: start uncalibrated, like a new board")
    ap.add_argument("--no-browser", action="store_true")
    a = ap.parse_args(argv)

    if a.list_ports:
        try:
            for dev, desc, vid in list_ports():
                print(f"{dev}\t{desc}\tVID={vid:04X}" if vid else f"{dev}\t{desc}")
        except ImportError:
            print("pyserial is not installed: pip install pyserial")
            return 1
        return 0
    if not (0 <= a.start_pct < a.stop_pct <= 100):
        ap.error("need 0 <= --start-pct < --stop-pct <= 100")

    if a.sim:
        kind, synthetic = "simulator", True
    elif a.replay:
        kind = "replay"
        synthetic = "synthetic" in a.replay.name.lower() or _file_says_synthetic(a.replay)
    else:
        kind, synthetic = "serial", False
        try:
            import serial  # noqa: F401
        except ImportError:
            print("pyserial is not installed: pip install pyserial   (or use --sim)")
            return 1

    station = Station(kind, synthetic, None if a.no_log else a.log_dir, a.start_pct, a.stop_pct)
    if a.sim:
        source = SimSource(station.on_line, station.on_status, a.speed, a.sim_verified, not a.sim_fresh)
    elif a.replay:
        source = ReplaySource(station.on_line, station.on_status, a.replay, a.speed, a.loop, synthetic)
    else:
        source = SerialSource(station.on_line, station.on_status, a.port, a.baud)
    station.send_fn = source.send

    httpd = make_server(station, a.host, a.http_port)
    url = f"http://{'127.0.0.1' if a.host in ('0.0.0.0', '') else a.host}:{a.http_port}/"
    print(f"dashboard: {url}   source: {kind}{' (SYNTHETIC)' if synthetic else ''}")
    if station.log_path:
        print(f"logging to {station.log_path}")
    source.start()
    if not a.no_browser:
        webbrowser.open(url)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        source.stop()
        httpd.server_close()
        station.close()
    return 0


def _file_says_synthetic(p: Path) -> bool:
    try:
        with open(p, encoding="utf-8") as f:
            return "synthetic" in f.read(4096).lower()
    except OSError:
        return False


if __name__ == "__main__":
    sys.exit(main())

"""Convert a dashboard session CSV into the `t_s,s1,s2,s3,pump` format analysis/analyze_log.py reads.

  python software/to_analysis_csv.py logs/session_20261029_101500.csv data/runs/2026-10-29-steady/steady.csv

t_s is board uptime in seconds, relative to the first record. s1..s3 are the firmware's `raw` counts
(each already an average of 12 ADC reads). pump is the software pump command.
A board restart in the middle of the log is refused, because t_s would jump backwards.
"""
import csv
import sys


def convert(src, dst):
    with open(src, newline="", encoding="utf-8") as f:
        lines = [l for l in f if not l.startswith("#")]
    synthetic = open(src, encoding="utf-8").readline().startswith("# SYNTHETIC")
    rows = list(csv.DictReader(lines))
    if not rows:
        raise SystemExit("no records in " + src)
    t0, prev, n = int(rows[0]["ms"]), -1, 0
    with open(dst, "w", newline="", encoding="utf-8") as f:
        if synthetic:
            f.write("# SYNTHETIC DATA, not a measurement\n")
        w = csv.writer(f)
        w.writerow(["t_s", "s1", "s2", "s3", "pump"])
        for r in rows:
            ms = int(r["ms"])
            if ms < prev:
                raise SystemExit(f"board restarted at ms {ms} (after {prev}): split the log there first")
            prev = ms
            w.writerow([round((ms - t0) / 1000), r["raw1"], r["raw2"], r["raw3"], r["pump_commanded"]])
            n += 1
    return n


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    print(f"wrote {convert(sys.argv[1], sys.argv[2])} records to {sys.argv[2]}")

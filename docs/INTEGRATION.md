# Hardware to software integration (v1.0)

## What is connected to what

| Layer | File | Role |
|---|---|---|
| Hardware | the board wired per `docs/handoff/Smart_Irrigation_DSP_Software_Handoff.md` section 3 | ESP32-S3 N16R8, 3 capacitive sensors on board labels 1/2/3, LED 5, relay IN 6, ILI9341 (RST 8, DC 9, CS 10, MOSI 11, SCK 12) |
| Firmware | `firmware/smart_irrigation/smart_irrigation.ino` | The handoff's `Smart_Irrigation_ESP32S3_Final.ino`, **byte for byte** (SHA-256 `2b1b87c3...addf9a`). Samples, filters, calibrates, controls the pump, draws the TFT, prints the telemetry |
| Link | USB serial, 115200, JSON Lines protocol v1 | Defined by the firmware. Documented in `software/README.md` |
| Software UI | `software/irrigation_ui/` | Reads the serial port. Dashboard, alerts, CSV log, analysis, confirmed commands |
| Analysis | `software/to_analysis_csv.py` then `analysis/analyze_log.py` | Report tables and figures from logged sessions |

The firmware was not changed. Every mismatch found is listed below instead, as the handoff asks.

## How the link is verified without the board

`make app-test` compiles the real .ino on the PC with stubbed hardware, drives a scripted scenario,
captures what it prints over serial, and runs that through the app's parser, history, gap detection and
alerts. It also checks that the relay pin is never driven ON while `PUMP_HARDWARE_VERIFIED=false`, and
that the app's line formatter reproduces the firmware's lines exactly. This proves the two sides agree on
the protocol. It does not prove the hardware works.

## Mismatches and findings (not fixed in the firmware, decide before relying on them)

1. **Two hardware builds exist in this repo.** `firmware/soil_dsp/` (and the README's older "Hardware
   integration" table and Cirkit schematic) targets ST7789, sensors on GPIO2/1/3 with S2/S3 = v2.0,
   relay on GPIO20, no LED. The current board follows the handoff: ILI9341, sensors 1/2/3 all v1.2, LED 5,
   relay 6. `soil_dsp` will not work on the current board. It is kept as the previous iteration.
2. **The report's DSP pipeline is not what the board runs.** The report title and `docs/WORKFLOW.md`
   describe a cross-sensor median, then a 5-point running median, then an EMA with alpha 0.1. The
   current firmware averages 12 ADC reads, then applies an EMA with alpha 0.25, with no median stage, and
   controls the pump from the integer mean of the three percentages. The dashboard can show the
   median+EMA filter for comparison, but only on the PC: it does not control anything. Either the report
   describes the firmware that actually runs, or the firmware changes. Do not present the PC-side filter
   as the controller.
3. **A manual `T` pulse can be cut short.** `controlWatering()` runs every second, before the AUTO
   check. If the board is uncalibrated, or the average is already at 70% or more, it calls
   `setPump(false)`, so a `T` pulse lasts between 0 and 1 s instead of 1.5 s (reproduced on the PC with the flag set true: 490 ms). It only matters once
   `PUMP_HARDWARE_VERIFIED=true`. Calibrate first and test `T` below 70%.
4. **One bad probe blanks all three.** `sensorFault` is a single flag. When any one sensor sits near 0
   or 4095, all percentages become null and AUTO is turned off. That is safe, but monitoring of the other
   two probes stops too. The dashboard still shows all three raw values, so the faulty one is visible.
5. **Calibration capture is short.** `D`/`W` average 20 reads over about 100 ms, with no settling time.
   Leave the probes still until the filtered values on the dashboard stop moving, then send D or W.
6. **The average is rounded down.** `averagePercent()` uses integer division, so 49.7% displays as 49%.
   This makes watering start at most 1 point early.
7. **The relay on pin 6 must trigger at 3.3 V.** With relay VCC at 5 V and `RELAY_ACTIVE_LOW=true`, a
   3.3 V HIGH may not fully release some optocoupler modules. This would look like the "reversed" pump
   behaviour already reported, so check it together with the COM/NO wiring.
8. **GPIO3 is a strapping pin.** The handoff already notes this for the PCB: consider moving S3 to label 4.
9. **Core version.** The handoff reports arduino-esp32 3.3.12 on the user's PC. CI compiles with 3.0.7,
   the version the older firmware was verified on. A clean CI compile is a good sign, not a proof for 3.3.12.
10. **The handoff's synthetic JSONL has time gaps** (ms 2000 to 10000, 11000 to 13000, 13000 to 35000).
    Replaying it shows 29 missing records. That is the gap detector working, not a fault in the app.

## Still not verified (needs the real board)

Supply voltages, relay COM/NO wiring and trigger level, LED polarity, ILI9341 power and backlight,
real calibration in the actual medium, thresholds for the crop, and any measured number for the report.
`data/synthetic/` and anything from the simulator are labelled SYNTHETIC and must not be quoted.

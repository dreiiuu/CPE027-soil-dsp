# Hardware to software integration (firmware v2.0)

## What is connected to what

| Layer | File | Role |
|---|---|---|
| Hardware | wired per `docs/handoff/Smart_Irrigation_DSP_Handoff_Automatic_v2.md` section 3 | ESP32-S3 N16R8, 3 capacitive v1.2 sensors on board labels 1/2/3, LED 5, relay IN 6, ILI9341 (RST 8, DC 9, CS 10, MOSI 11, SCK 12) |
| Firmware | `firmware/smart_irrigation/smart_irrigation.ino` (v2.0) | Samples, filters, calibrates, controls the pump, draws the TFT, prints the telemetry |
| Link | USB serial, 115200, JSON Lines protocol v1 | One telemetry line per second, `#EVENT` lines, single-letter and `$` commands. See `software/README.md` |
| Software UI | `software/irrigation_ui/` | Reads the serial port. Dashboard, alerts, CSV log, analysis, calibration from raw values, bench test input |
| Analysis | `software/to_analysis_csv.py` then `analysis/analyze_log.py` | Report tables and figures from logged sessions |

### Where firmware v2.0 came from

The v2 handoff describes `Smart_Irrigation_ESP32S3_Automatic_v2.ino`, but that file was **not supplied**.
The v2.0 in this repo was written here: v1.0 (`git show 7b90401:firmware/smart_irrigation/smart_irrigation.ino`,
the handoff's original, SHA-256 `2b1b87c3...addf9a`) plus the behaviour the v2 handoff specifies, plus the
raw-value tools below. The pinout, the protocol version, the 50/70 thresholds, the pulse, soak and pulse limits,
the EMA, the fault limits and `PUMP_HARDWARE_VERIFIED=false` are unchanged. If you have the original v2 file,
diff it against this one before flashing.

| v2 handoff requirement | Where |
|---|---|
| AUTO by itself after power-on (calibration valid, sensors good, hardware verified, not paused or locked out) | `autoAllowed()`, `setup()` |
| 5 s settling before the first pulse | `BOOT_SETTLE_MS`. Also applied after a new calibration (added: the probes were just handled) |
| PAUSE and LOCKOUT survive a reboot | NVS keys `paused`, `lockout` |
| `X` clears lockout and pauses; `A` resumes; `W` after `D` arms AUTO | `handleSerial()`, `storeCalibration()` |
| Sensor fault: pump off, AUTO paused until `A` | `controlWatering()` |
| Lockout only after the third pulse **and** its soak, if still below the stop level | `controlWatering()` |
| `paused` in the telemetry | `emitTelemetry()` |
| TFT shows CALIBRATE | `updateDisplay()` |

### Added for easier testing with raw values

| Addition | Why |
|---|---|
| `$CAL d1 d2 d3 w1 w2 w3` | Store dry/wet references from known raw readings. The dashboard proposes them from the last 30 s of filtered readings and says whether the probes have settled. No more re-capturing D/W just to fix one number. The firmware applies the same limits as D/W: no rail values, span of at least 150 counts per sensor |
| `$RAW r1 r2 r3`, `$RAW OFF` | Bench test input. The board uses these raw values instead of the sensors, for at most 5 minutes. The real control path runs unchanged (EMA, %, thresholds, pulses, soak, lockout, LED, TFT), so the pump reaction at a chosen % can be tested without drying or flooding the soil. Flagged as `test_input` in the telemetry and as TEST INPUT on the TFT. Ended by `$RAW OFF`, `X` or the timeout |
| `dry_ref`, `wet_ref`, `test_input` telemetry fields | The PC can see the references the board is actually using, check its % maths, and show when a test input is active. These are additive fields; `v` stays 1 and v1.0 logs still parse |
| `T` runs its full 1.5 s | v1.0 cut a manual pulse short when uncalibrated or at 70% or more (measured 490 ms) |

## How the link is verified without the board

`make app-test` compiles the real .ino on the PC with stubbed hardware, twice:

* **As shipped** (`PUMP_HARDWARE_VERIFIED=false`): boot, D/W, `$CAL` (accepted and rejected), dry soil, the
  A/T denials, a disconnected probe, `$RAW`, LED test, X, a power cycle. The relay pin must never turn on.
* **With `PUMP_HARDWARE_VERIFIED=true`** (a generated copy in `build/`): `$CAL` arms AUTO; nothing pumps
  during the 5 s settling; then 3 pulses of 1.5 s at least 20 s apart, then lockout; lockout survives a reboot
  and `A` cannot clear it; `X` pauses and the pause survives a reboot; `T` runs 1.5 s; AUTO starts by itself
  after a reboot and waits 5 s; `$RAW` at 45% triggers a pulse; a sensor fault pauses AUTO until `A`.
  The relay pin and the reported pump state must agree at every 10 ms step.

Both serial outputs then go through the app's parser, history, gap and reboot detection and alerts. The app's
formatter has to reproduce every firmware line byte for byte.

The simulator (`--sim`) is a Python port of the same logic. Its own tests check the same rules, but it is not
proven identical to the C++. The C++ is the authority.

## Mismatches and findings (decide before relying on them)

1. **The report's DSP pipeline is not what the board runs.** The report title and `docs/WORKFLOW.md` describe
   a cross-sensor median, then a 5-point running median, then an EMA with alpha 0.1. The firmware averages
   12 ADC reads, then applies an EMA with alpha 0.25, with no median stage, and controls the pump from the
   integer mean of the three percentages. The dashboard shows median+EMA only for comparison, on the PC.
   Either the report describes the firmware, or the firmware changes. Do not present the PC-side filter as
   the controller.
2. **Two hardware builds exist in this repo.** `firmware/soil_dsp/` (and the README's older "Hardware
   integration" table and Cirkit schematic) targets ST7789, GPIO2/1/3 with v2.0 sensors, relay GPIO20, no LED.
   It does not match the current board and is kept only as the previous iteration.
3. **With AUTO_START on, "manual" means "paused".** `T` is refused while AUTO runs, so for a supervised single
   pulse: `X` (pause), `T`, then `A` to resume. MANUAL (not paused, not AUTO) only appears while
   `PUMP_HARDWARE_VERIFIED=false`.
4. **`D` alone pauses AUTO.** Re-doing only the dry reference leaves AUTO paused until `W`, `$CAL` or `A`.
5. **One bad probe blanks all three.** `sensorFault` is a single flag: any sensor near 0 or 4095 nulls all
   percentages and pauses AUTO. That is safe, but monitoring of the other two stops too. The dashboard still
   shows all three raw values.
6. **Calibration capture with D/W is short** (20 reads in about 100 ms). Wait until the dashboard says "settled",
   or use the 30 s median and `$CAL` instead.
7. **The average is rounded down** (integer division), so 49.7% displays as 49%.
8. **`$RAW` really drives the pump** once the hardware is verified. It is a bench tool: outlet into a cup,
   someone watching. Every safety rule still applies, and it ends by itself after 5 minutes.
9. **The relay on pin 6 must trigger at 3.3 V.** With relay VCC at 5 V and active LOW, a 3.3 V HIGH may not fully
   release some optocoupler modules. That would look like the "reversed" pump already reported, so check it
   together with the COM/NO wiring.
10. **GPIO3 is a strapping pin.** Consider label 4 for S3 on the PCB.
11. **Core version.** The handoff reports arduino-esp32 3.3.12. CI compiles with 3.0.7. A clean CI compile is a
    good sign, not a proof for 3.3.12. Compile once in your own IDE.
12. **The handoff's synthetic JSONL has time gaps.** Replaying it shows 29 missing records. That is the gap
    detector working.

## Still not verified (needs the real board)

Supply voltages, relay COM/NO wiring and trigger level, LED polarity, ILI9341 power and backlight, the real
dry/wet references in your medium, thresholds for the crop, and any measured number for the report.
`data/synthetic/` and anything from the simulator are labelled SYNTHETIC and must not be quoted.

## Bench procedure for the pump (suggested order)

1. Pump supply disconnected. Flash with `PUMP_HARDWARE_VERIFIED=false`. Open the dashboard and check that all
   three sensors show raw values that move when you touch the probes.
2. Calibrate: probes in dry soil, wait for "settled", **Use last 30 s as DRY**. Then the same in wet soil with
   **Use last 30 s as WET**. Then **Send to board**.
3. Relay check with a multimeter on COM/NO, pump still unplugged: verify that NO closes only while the dashboard
   shows Pump ON. Then set `PUMP_HARDWARE_VERIFIED=true` and flash again.
4. Pump outlet into a cup. **Bench test input** at 75%: no pulse. At 45%: one 1.5 s pulse, then 20 s of soak,
   then the next one, and lockout after the third (the test input does not get wetter). Then **X**, **End test**.
5. Only then let it run on the real probes, supervised.

# Soil-moisture DSP: three capacitive sensors, median + EMA smoothing, web dashboard

CPE 027 DSP project: *Two-Stage Median and Exponential Smoothing of Multi-Sensor Soil Moisture Signals for Tomato Germination Monitoring* (ESP32-S3).

```
3 capacitive sensors -> ADC (1 Hz) -> calibrated index -> cross-sensor median
   -> 5-point running median -> EMA (alpha 0.1) -> thresholds + hysteresis -> pump / TFT / web dashboard
                                  \-> M=10 moving average (comparison baseline, same input)
```

## Current build (v1.0): start here

The board on the bench follows `docs/handoff/Smart_Irrigation_DSP_Software_Handoff.md`: ILI9341 TFT, three
v1.2 sensors on board labels 1/2/3, red LED on 5, relay on 6. Its firmware is
**`firmware/smart_irrigation/smart_irrigation.ino`** v2.0: automatic watering per the v2 handoff, plus calibration
from typed raw values (`$CAL`) and a bench test input (`$RAW`). The pump stays blocked until you set
`PUMP_HARDWARE_VERIFIED=true`. The sensor values reach the software UI over USB serial:

```bash
pip install -r software/requirements.txt
```

```bash
cd software && python -m irrigation_ui --port COM7
```

Then open http://127.0.0.1:8765/ (add `--sim --sim-verified` instead of `--port` to try it without the board).
Details: `software/README.md`. How the pieces connect, and the firmware/report mismatches to resolve:
**`docs/INTEGRATION.md`**.

The sections below, from "Hardware integration" to "First run", describe the **previous build**
(`firmware/soil_dsp`: ST7789, relay on GPIO20, Wi-Fi dashboard). That firmware does not match the current board.
The analysis tooling still applies: convert a session log with `software/to_analysis_csv.py` first.

## What is in the repo

| Path | What it is |
|---|---|
| `firmware/soil_dsp/dsp.h` | The DSP core (calibration, median3, running median, EMA, moving average, pump controller, toggle counter). No Arduino dependencies. |
| `firmware/smart_irrigation/smart_irrigation.ino` | **Current firmware** v2.0: 3 sensors, 12x average + EMA, calibration (D/W or `$CAL`), automatic watering with pause/lockout kept in NVS, `$RAW` bench test, ILI9341 TFT, JSONL telemetry over USB. |
| `software/irrigation_ui/` | **Current software UI**: PC app that reads the serial telemetry. Live dashboard, alerts, CSV log, DSP comparison, confirmed commands, simulator. |
| `docs/INTEGRATION.md` | How hardware, firmware and software connect, and the mismatches found. |
| `firmware/soil_dsp/soil_dsp.ino` | ESP32-S3 firmware: samples 3 sensors, runs the pipeline, drives relay/TFT, serves the dashboard, logs CSV over serial. |
| `firmware/soil_dsp/dashboard.h` | Web dashboard (raw vs smoothed plot, status word, 3 sensor readings, toggle counters, calibration, CSV download). |
| `firmware/legacy_readout/` | Your original 3-sensor read-out sketch (iteration 0, kept for reference). |
| `analysis/analyze_log.py` | Computes Tables 6-1, 6-2, 6-3, the pump-interference table, Fig 6-1, Fig 6-2 from the logged CSV. |
| `analysis/dsp_ref.py` | Python version of the same filters. `crosscheck.py` proves it matches the firmware C++. |
| `firmware/prebuilt/` | Ready-to-flash image (compiled with esp32 core 3.0.7, ESP32S3 Dev Module defaults). Use it if the Arduino IDE gives you trouble. |
| `tests/` | C++ unit tests for `dsp.h`, a host simulation that runs the real `.ino` against scripted sensor values, and a browser check of the dashboard (`dashboard_mock.py` + `dashboard_check.js`). |
| `docs/WORKFLOW.md` | The final DSP thought process (slide-ready) and the 3-sensor version of the problem statement/objectives. |
| `docs/PAPER_PATCHES.md` | Exact edits and fill-in mapping for the DSP report. |
| `docs/ITERATIONS.md` | Iteration plan, milestones, branch model. `scripts/bootstrap_github.sh` creates the issues and milestones. |

## Hardware integration (previous build, `firmware/soil_dsp`)

Wiring matches your Cirkit schematic and the working read-out sketch.

| Function | ESP32-S3 pin | Notes |
|---|---|---|
| Sensor 1 (capacitive **v1.2**) AOUT | GPIO2 (ADC1) | 3.3 V supply, GND |
| Sensor 2 (capacitive **v2.0**) AOUT | GPIO1 (ADC1) | 3.3 V supply, GND |
| Sensor 3 (capacitive **v2.0**) AOUT | GPIO3 (ADC1) | 3.3 V supply, GND |
| TFT (ST7789 240x320, SPI) | SCK 12, MOSI 11, CS 10, DC 14, RST 21 | VCC 3.3 V, backlight on |
| Relay IN (active LOW module) | GPIO20 | Relay VCC 5 V. Swap `RELAY_ON/OFF` if your module is active HIGH. |
| Pump | relay COM/NO from the 5 V supply | Flyback diode (1N4007) across the pump, bulk capacitor across the pump supply |

Checks before powering up:

1. **Set the breadboard PSU to 5 V, not 6 V.** The schematic is annotated "working (but breadboard psu is set to 6v)". Relay modules and the ESP32 5 V input are specified for 5 V; 6 V works until it doesn't, and the sensors' regulated 3.3 V rail is the thing the ADC counts depend on.
2. **ADC1 only.** GPIO1 to GPIO10 are ADC1. ADC2 stops working while Wi-Fi is on, so the sensors cannot be moved to GPIO11 to GPIO20.
3. **GPIO20 is the native-USB D+ line on ESP32-S3 boards.** It works in your prototype. If serial or flashing misbehaves while the relay is wired, flash/log through the **UART** USB port, or move the relay to another free GPIO and edit `RELAY_PIN`.
4. **Probe placement (this decides the quality of the data).** Same insertion depth (to the marked line) for all three, at least 3 cm apart and away from the tray wall, electronics above the soil line kept dry, sensor wires routed away from the pump wires. The cross-sensor median only helps if the three probes see the same bulk moisture.
5. **Mixed sensor models.** S1 is a v1.2 and S2/S3 are v2.0, so their ADC ranges differ. That is exactly what per-sensor calibration (Eq. 2) corrects, and Table 6-1 will show the spread. State the models in the report.
6. **Test 4 (pump interference):** run the pump into a separate cup, so the probes do not get wetter. Otherwise real moisture change is mixed into what the script counts as interference.

## First run (20 minutes)

1. Arduino IDE (or arduino-cli) with the **esp32 core 3.x** (verified on 3.0.7); install libraries *Adafruit GFX Library* and *Adafruit ST7735 and ST7789 Library*. In a hurry? Flash `firmware/prebuilt/` instead (see its README).
2. Optional: copy `firmware/soil_dsp/secrets.example.h` to `secrets.h` and fill in your Wi-Fi. With no `secrets.h` (or no network) the board creates its own access point: SSID `SoilDSP`, password `soildsp123`, page at `http://192.168.4.1/`.
3. Board: *ESP32S3 Dev Module*, leave **USB CDC On Boot: Disabled** (the default). Plug into the board's **UART/COM** USB port, not the native **USB** port: GPIO20 (the relay) is the native port's D+ line, and with CDC disabled `Serial` goes to the UART port. Open `firmware/soil_dsp/soil_dsp.ino`, upload, open the serial monitor at 115200.
4. The pump is **disabled at boot** and locked until both calibrations exist. Press `d` in the serial monitor with all three probes in air, then `w` with all three in water up to the line (60 s each; the same buttons are on the dashboard). Values are stored in flash.
5. Open the dashboard (IP is on the TFT and in the serial log). Enable pump control from the dashboard or with `e` once the thresholds are right for your medium.

Serial commands: `d`/`w` calibrate dry/wet, `e`/`x` enable/disable pump control, `t` 3 s on/3 s off pump test, `b` blanking on/off, `r` reset filters, `s` show parameters, `h` CSV header, `p` pin map + wiring check.

**Wiring check (do this before calibrating).** At boot the serial log prints the pin map the firmware uses (`# pins: ...`). The dashboard's sensor table shows each sensor's GPIO, raw counts and a *Wiring* column; the TFT row turns orange. A sensor is flagged after 3 s in a row of:

- `NO SIGNAL` (under 200 counts): AOUT not on the pin it should be on, or the probe has no VCC/GND. Check the jumper against the table above.
- `AT 3.3V` (over 4000 counts): AOUT shorted to 3.3 V, or a 5/6 V rail is reaching the ADC pin. **Unplug it immediately**: ESP32-S3 pins are not 5 V tolerant.

A probe in air should read about 2700-3000 counts and one in water about 1100-1400. If S1 moves when you touch S2, two AOUT wires are swapped. Then press `t` and listen for the relay clicking every 3 s, which confirms GPIO20 to relay IN.

## Evidence run (the tests the report needs)

Log the serial output to a file. Any of these works:

```bash
python3 -m serial.tools.miniterm /dev/ttyUSB0 115200 | tee data/steady.csv    # pip install pyserial
# or PuTTY/Arduino serial monitor logging, or the dashboard's "Download CSV" button
```

Then, per test (the script ignores `#` comment lines and the header):

```bash
# Test 1: 60 s in air, 60 s in water (record each as its own log)
python3 analysis/analyze_log.py calib  --air data/air.csv --water data/water.csv --out data/calibration.json

# Test 2: >= 30 min undisturbed in moist medium, pump disabled
python3 analysis/analyze_log.py steady --log data/steady.csv --cal data/calibration.json --out results

# Test 3: note the t_s at which you pour the measured water
python3 analysis/analyze_log.py step   --log data/step.csv   --cal data/calibration.json --t-step 300 --out results

# Test 4: pump cycling (press t), with probes not getting wetter
python3 analysis/analyze_log.py pump   --log data/pump.csv   --cal data/calibration.json --out results
```

Outputs in `results/`: `table_6-2.md`, `table_6-3.md`, `table_pump.md`, `steady_timeseries.png` (Fig 6-1), `steady_spectrum.png` (Fig 6-2), `filter_responses.png`, `step_response.png`, `pump_interference.png`, plus the JSON versions. Table 6-1 is printed by `calib`.

Set the calibration file's `theta_dry` and `blank_s` to the values the firmware used (dashboard or `s` shows them) so the script reproduces the same decisions.

## How the measures are defined (state these in the report)

- **Std. dev. / peak-to-peak**: over the steady window after a 60 s warm-up.
- **Spike**: a sample that differs from the centred 5-point median of the same signal by more than `--spike-thr` index units (default 5.0). One absolute threshold for every signal.
- **Toggles at θ**: how many times `signal >= θ` changes. Plain crossing count, no hysteresis, so the filter's effect is not hidden by decision hysteresis. Default θ is the median of the steady signal (the worst case, where the moisture sits on the threshold); pass `--theta 35` for the fixed θdry.
- **t90**: time after the water was added until the signal reaches 90% of the way from its pre-step mean to its final mean and stays there for 5 samples. Raw t90 includes real water-spreading time in the medium.
- **Pump interference**: samples within 5 s after each relay switch deviating more than the threshold from the median of the 10 samples before the switch.

The script also prints an extra row, a moving average with M = 19. An EMA with α = 0.1 has the same white-noise gain as a 19-point moving average, so comparing against M = 10 alone is not like for like. Report both and say which is which.

## Verify before you trust it

```bash
make test        # C++ unit tests, firmware host simulation, synthetic end-to-end, firmware/python cross-check
make dashboard   # optional: real dashboard page in Chromium against a replayed log (needs node + playwright)
```

What has been verified, and what has not:

| Check | Status |
|---|---|
| DSP core unit tests, host simulation of the `.ino`, firmware vs Python cross-check | pass |
| Compile for ESP32-S3 with the real Espressif toolchain (core 3.0.7, `--warnings all`) | pass, 0 warnings, 75% flash, 15% RAM |
| Dashboard in Chromium (laptop and phone width), no JS errors, live update | pass (against a mock replay, `docs/img/`) |
| Wiring check (fault flags, pin map in UI/serial) | pass in host simulation; **the prebuilt image predates it**, rebuild from source to get it |
| PC app: parser, history, gap and reboot detection, alerts, commands, simulator, HTTP API, fed with the real `smart_irrigation.ino` output run on the PC (`make app-test`) | pass |
| PC app in Chromium against the simulator, laptop and phone width (`make app-ui`) | pass |
| `smart_irrigation.ino` compiled for ESP32-S3 by CI | runs on push to `main` or on a PR, not yet run |
| Running on your board, sensors, relay, pump | **not yet: that is iteration v0.5** |

`data/synthetic/` is **simulated**, only there so the pipeline and CI can run before you have measurements. Never quote numbers from it.

## Git and iteration process

See `docs/ITERATIONS.md`. Short version: `main` is always green (CI runs `make test` and compiles the firmware), work happens on `feat/*` branches through pull requests, each iteration is a milestone with issues created by `scripts/bootstrap_github.sh`, tagged `v0.x`.

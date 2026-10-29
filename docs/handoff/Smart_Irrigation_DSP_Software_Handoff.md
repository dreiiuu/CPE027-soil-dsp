# CONTEXT AND IMPLEMENTATION PROMPT FOR DSP / SOFTWARE DEVELOPMENT

I am building an **ESP32-S3 N16R8-based soil moisture monitoring and automatic irrigation prototype**. Please use the details in this handoff as the project baseline, and use the attached **`Smart_Irrigation_ESP32S3_Final.ino`** as the source of truth for firmware behavior and telemetry. Do not assume hardware or features have been proven unless explicitly marked as verified.

## 1. Project goals
- Measure soil condition at three locations using three **capacitive soil moisture sensor v1.2** modules.
- Process and filter their readings on the ESP32-S3.
- Display live readings, relative moisture percentages, pump status, safety warnings and operating mode continuously on a **2.4-inch, 240 x 320 SPI TFT** (assumed ILI9341 controller).
- Automatically switch **one 5V DC mini water pump** via a **5V one-channel relay module** when calibrated soil moisture indicates watering is necessary.
- Use a **red external LED** as a visual low-moisture/fault alert instead of a buzzer.
- Link the hardware to a future **DSP / computer software interface** for live monitoring, logging, trend analysis, visualization, and potential alerting. Software environment and programming language are not yet specified. Do not invent an installed application, protocol, or network service.

## 2. Verified hardware and development status
- Initially considered ESP8266, but rejected it for the final design: it has only one analog input on typical development boards and would need an external ADC or multiplexer for three sensors.
- Considered ESP32-S2 during initial Arduino installation, but **actual detected chip is ESP32-S3 revision v0.2**, according to a successful Arduino upload on Windows **COM7**. The log reported **8MB embedded PSRAM**. The selected purchasing variant is ESP32-S3 N16R8 with soldered headers (nominally 16MB flash and 8MB PSRAM).
- Arduino IDE package **esp32 by Espressif Systems** was installed; upload to the ESP32-S3 succeeded with esptool. Serial output successfully showed repeated `ESP32-S3 PROGRAM RUNNING` messages.
- A prior combined hardware test succeeded in receiving raw readings from all three sensors and illuminating/operating the TFT. Example raw readings: **S1 around 2575–2581; S2 around 3158–3176; S3 around 3190–3204**. These are raw 12-bit ADC counts, not soil moisture percentages.
- TFT backlight is illuminated, and the user reports the TFT is working. A previously implemented screen redraw cleared the whole TFT each second and caused undesired flicker. New firmware redraws only changed value regions; title and labels stay visible.
- User confirmed the relay/pump operated but **pump flow was reversed relative to software ON/OFF messages**. The most likely causes are using relay **NC** instead of **NO**, or mismatched relay trigger polarity. This is **NOT verified corrected**. The final firmware intentionally sets `PUMP_HARDWARE_VERIFIED=false` to block energizing/automatic watering until wiring is fixed and tested.
- External red LED previously did **not light**. Firmware includes a 2-second startup LED test and `L` command, but physical LED/resistor polarity and row connections still need to be checked. This is **NOT verified fixed**.
- Prior readings around 2.22V on an intended 3.3V circuit and 2.42V on an intended 5V circuit were reported; measurement technique was in question (AC setting was briefly used). Correct DC power measurements have **NOT been conclusively documented**. Confirm correct supply voltages before attaching pump power.
- The user has not installed all planned optional capacitors and resistors; prototypes ran partially without them.
- Final etched PCB is **not yet fabricated**; system is presently prototyped with two breadboards.

## 3. Exact ESP32-S3 board-label pin assignments
The numbers below refer to the labels printed at the ESP32 development board headers (not header position numbers):

| Hardware | Module pin | ESP32-S3 board label |
|---|---|---|
| Capacitive sensor 1 | AO | `1` |
| Capacitive sensor 2 | AO | `2` |
| Capacitive sensor 3 | AO | `3` (GPIO3 is a boot strapping pin; for a future PCB, consider moving this sensor to label `4` and update firmware) |
| All three sensors | VCC/GND | 3V3 / common GND |
| External red LED | anode via 220–470 ohm resistor | `5` |
| LED | cathode | GND |
| 5V relay module | IN | `6` (3.3V trigger compatibility required) |
| TFT | RESET | `8` |
| TFT | D/C | `9` |
| TFT | CS | `10` |
| TFT | SDI(MOSI) | `11` |
| TFT | SCK | `12` |
| TFT | SDO(MISO) | not connected; not needed for writing display |
| TFT | LED / BL backlight | follow actual TFT module's voltage/current specification; not assumed to be a logic GPIO |
| TFT | VCC / GND | supported 3.3V supply (verify module) / common GND |

Relay module VCC connects to the external **regulated 5V** supply, relay GND to common ground on an ordinary non-isolated relay module. Relay screw contacts: **5V supply positive -> COM -> NO -> pump positive**, pump negative -> supply negative, and **NC left unused**. The pump is controlled ONLY through relay contacts, not through a soil sensor output or ESP32 GPIO. Add adequate flyback suppression for a brushed DC pump, subject to motor current. Confirm relay contact DC rating and pump startup current.

## 4. Power architecture
- **Breadboard 1:** USB powers the ESP32-S3; the ESP32's 3V3 output (subject to regulator limits) supplies compatible sensors and logic. TFT power must match module specification.
- **Breadboard 2:** External regulated **5V DC adapter** powers the relay and pump circuit. **5.5 x 2.1mm female PCB-mounted DC barrel jack** is planned for etched PCB.
- Join the **GND** between the two breadboards, but DO NOT tie their positive 3.3V and 5V rails together; DO NOT tie external 5V directly to a USB-powered board's 5V rail unless board-specific power OR-ing allows it.
- Do not run the pump's operating current through breadboard power rails; route through appropriately rated terminals and wiring.
- Desired additional decoupling BOM: one **470uF, 16V electrolytic** across external 5V and GND; optional two **10uF (10V+) electrolytics** (one 3.3V rail, one 5V rail); optional three **100nF ceramic** capacitors close to sensor power connectors. Observe electrolytic polarity.
- LED requires a **220-470 ohm series resistor** (or 1k ohm if necessary). The originally planned 10k resistors were conditional, not required in the proven circuit; other LED resistor values must not be omitted.
- On final PCB use female headers matched to actual ESP32 pin-row counts, 3-pin sensor connectors, appropriate TFT/relay connectors, pump screw terminals, switch, fuse, and the DC jack.

## 5. Firmware behavior (attached .ino v1)
- **Boot:** The relay is driven to its configured de-energized electrical state as soon as possible; automatic watering defaults to OFF. External LED is tested for 2 seconds at startup. The TFT is initialized and keeps its static title/labels. Automatic pump commands are blocked until an explicit code constant `PUMP_HARDWARE_VERIFIED` is changed to `true` after physical correction and supervised tests.
- **Sampling:** Every **1 second**, all three sensors are sampled. Each raw reading averages **12 ADC samples**, followed by an **exponential moving average (EMA) filter with alpha = 0.25**. ESP32 ADC is configured to 12-bit reads (0–4095) and high attenuation (`ADC_11db`). Raw and filtered ADC counts are exposed in telemetry.
- **Calibration:** Sending serial `D` saves a dry reference for every sensor; sending `W` saves a wet reference for every sensor. Values are stored in ESP32 Preferences/NVS and survive restarts. Require >=150 ADC count difference between dry and wet for each sensor. The calibration percentage is **relative to these two points**: `pct = clamp(100 * (filtered - dry) / (wet - dry), 0, 100)`. The formula works irrespective of the direction of raw changes. Missing or invalid calibration produces `null` percentages and the TFT displays CAL REQUIRED; this replaces old `AVG=-1` semantics in the machine-readable stream.
- **Mean moisture:** Equal-weight average of three calibrated moisture percentages. They are considered sensors for one shared watering area. Independent irrigation zones would need different pumps/valves and new logic.
- **Starting thresholds:** average below **50%** requests watering; average >= **70%** clears the request. Hysteresis prevents rapid relay switching. These are **adjustable prototype thresholds**, not validated volumetric water content; crop, soil and calibration must be studied. Irrigation extension literature recommends crop/soil-dependent management allowable depletion of plant-available water. That physical quantity is different from a two-point capacitive calibration percentage.
- **Watering sequence:** Automatic mode enabled manually by serial `A` only after valid calibration, healthy sensor reading and successful relay hardware verification. Pump turns on for a maximum **1,500 ms**, switches off, and waits at least **20,000 ms** for moisture redistribution. Up to **3 pulses** are permitted in a run; hitting the pulse limit latches `lockout=true`, disables automatic mode, and requires operator inspection/`X` to clear. The next `A` is required for re-enable.
- **LED:** Lights continuously while there is a calibrated low-moisture watering demand; blinks on obvious sensor faults (near ADC rails); `L` is a 2-second LED test.
- **Safety:** Sensor readings near ADC 0 or 4095 are classified as fault; relay commanded OFF and AUTO disabled. Not all faults are detectable. Pump/relay status is the **software command only**, not an independent flow measurement. Real dry-run / tank-empty detection is not implemented. This code is for supervised testing, not unattended deployment.
- **Display:** 320x240 landscape; fixed header and labels; only changing values are redrawn to avoid screen flashing. Shows S1/S2/S3 raw or relative %, average/diagnostics, pump command, mode and warning/pulse count.

## 6. Arduino programming environment
- Arduino IDE on Windows; detected ESP32 at **COM7**, baud **115200** for Serial Monitor.
- Generic board selection: **ESP32S3 Dev Module** (NOT Uno, NOT ESP32-S2). Earlier Arduino Uno setting generated AVR `not in sync` errors; correcting the board fixed upload.
- Libraries: **Adafruit GFX**, **Adafruit ILI9341**, built-in **Preferences**.
- Espressif arduino-esp32 installed, user log showed version **3.3.12**.
- Use matching flash/PSRAM board settings for N16R8; the upload log verified 8MB embedded PSRAM.

## 7. Serial protocol for DSP / computer dashboard
- Transport: USB CDC/serial, **115200 baud**, device port COM7 in previous tests (port can change).
- Each **telemetry** record is a complete **JSON object on one line** (JSON Lines / NDJSON). Parse only lines beginning with `{` and with `"type":"telemetry"`; other messages begin `#EVENT` for human diagnostics.
- Fields and semantics:
  - `type`: `"telemetry"`
  - `v`: protocol version number (1)
  - `ms`: milliseconds of microcontroller uptime; NOT an absolute datetime
  - `raw`: integer array of 3 unfiltered 12-bit ADC averages
  - `filtered_raw`: integer array of 3 EMA-smoothed ADC counts
  - `moisture_pct`: array of 3 integers 0–100, or null for a sensor when calibration is missing or faults invalidate readings
  - `average_pct`: integer 0–100 or null when calibration/validity is missing
  - `calibrated`, `sensor_fault`, `water_needed`, `pump_commanded`, `led_on`, `auto_mode`, `lockout`, `hardware_verified`: booleans
  - `pulses`: integer number of irrigation pulses in the current cycle
- Example only, NOT a real measurement:
  `{"type":"telemetry","v":1,"ms":60123,"raw":[2580,3160,3197],"filtered_raw":[2582,3163,3196],"moisture_pct":[44,48,52],"average_pct":48,"calibrated":true,"sensor_fault":false,"water_needed":true,"pump_commanded":false,"led_on":true,"auto_mode":true,"lockout":false,"pulses":1,"hardware_verified":true}`
- USB Serial commands, single letters (send newline if needed): `D` dry calibration, `W` wet calibration, `A` automatic enable, `X` stop/reset software lockout, `L` LED test, `T` supervised pump pulse, `?` help. Do not let GUI buttons send high-risk commands accidentally; require user confirmation and physical safety checks.

## 8. What I want you to develop on the software / DSP side
Please propose and implement a clean, modular computer-side application (ask about desktop/web environment only if essential):
1. Robust serial connection, auto reconnect and schema-version checking; parse JSONL and ignore/log `#EVENT` lines separately.
2. Realtime dashboard with the three raw/filtered sensor readings, each relative calibration percentage, average moisture, thresholds, water demand, ON/OFF command, LED status, manual/auto modes, pump pulse count, fault and lockout status.
3. Time-series plots and history logging (CSV or SQLite), PC reception timestamps (because ESP `ms` is uptime only), data export, missing-record detection.
4. DSP analysis options: moving average vs EMA, spike/outlier detection, calibration-quality and drift checks, rate of soil drying, water-response lag after a pump pulse, visualization of trends. Clearly separate optional dashboard-side filtering from the firmware's safety-critical water control.
5. Configurable threshold display and informative alerts. Don't falsely claim the 50/70 relative calibration thresholds equal crop depletion fractions or volumetric water content.
6. Connection and safety states: disconnected, uncalibrated, bad sensor, AUTO disabled, pump hardware not verified, lockout after maximum pulses.
7. A well-documented serial interface and realistic simulation mode using explicitly labeled synthetic sample data.
8. Do not claim to know actual pump flow, tank water level, or absolute soil moisture without additional sensors and calibration. Keep pump safety control local to ESP32 in this initial iteration.

## 9. Remaining physical checks / known limitations
- Confirm isolated ESP32 power is approximately 3.3V; external 5V supply approximately 5.0V DC. Earlier unusual values were not clearly resolved.
- Correct the reversed pump operation: COM->positive adapter, NO->pump positive, NC unused, pump negative->adapter negative. Verify relay signal polarity and 3.3V trigger support. The final .ino deliberately blocks running until this is confirmed.
- Confirm LED polarity, proper resistor value in series, and breadboard row placement; the LED had not lit in previous testing.
- Confirm the exact ILI9341 module voltage/backlight requirements and avoid overloading the ESP32's onboard 3.3V regulator.
- Check possible GPIO3 strapping restrictions in the final PCB.
- Select power adapter, fuse, relay-contact ratings, terminal wire gauges, and diode based on the pump's actual operating and startup current.
- No water-level sensor, flow sensor, cloud connection, Wi-Fi integration, multi-zone valves, validated field thresholds, or true unattended safety setup has yet been implemented.

## 10. Reliable reference resources
- Espressif ESP32-S3 ADC and pinout: https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html
- Espressif Arduino ADC reference: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html
- Espressif Arduino Preferences/NVS: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/preferences.html
- Adafruit ILI9341 class reference: https://adafruit.github.io/Adafruit_ILI9341/html/class_adafruit___i_l_i9341.html
- University of Minnesota Extension irrigation planning: https://extension.umn.edu/natural-resources/conservation/agricultural-soil-and-water/irrigation

Please treat the attached .ino file as the authoritative implementation and identify any software/hardware mismatch rather than silently changing the pinout, serial schema, or safety constraints.

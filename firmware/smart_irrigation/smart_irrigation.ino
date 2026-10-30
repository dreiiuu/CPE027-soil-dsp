/*
  SMART SOIL IRRIGATION - ESP32-S3 N16R8
  Version 2.1 - automatic, supervised prototype (v2.1: DSP-paper filter, DRY/WET status)

  Board header labels: Sensor 1=1, Sensor 2=2, Sensor 3=3
  LED=5 (220-470 ohm series resistor), Relay IN=6
  ILI9341 SPI: RESET=8, DC=9, CS=10, MOSI/SDI=11, SCK=12

  IMPORTANT:
  - Pump positive: relay NO; relay COM: +5V pump adapter;
    pump negative: adapter GND; NC is unused.
  - Control GND and ESP32 GND are common on ordinary relay modules.
  - NEVER connect the pump directly to an ADC pin or ESP32 GPIO.
  - Test relay NO/COM and active level with pump supply disconnected.
  - Set PUMP_HARDWARE_VERIFIED=true only after correcting reversed behavior.
  - Keep automatic operation supervised; no water-level sensor is installed.

  What changed from v1.0:
  - AUTO starts by itself after power-on when calibration is valid, sensors are good,
    PUMP_HARDWARE_VERIFIED=true and the system is not PAUSED or LOCKED OUT.
    The first pulse waits 5 s after boot (and 5 s after a new calibration).
  - PAUSE (X, sensor fault, dry calibration) and LOCKOUT are stored in NVS and survive a reboot.
    A resumes. A completed wet calibration (W, or $CAL) arms AUTO again.
  - Lockout is declared after the third pulse AND its soak time, if the soil is still below the stop level.
  - A manual T pulse always runs its full 1.5 s (v1 could cut it short within a second).
  - Calibration can be typed in from known raw readings ($CAL), and raw test values can be
    injected for bench tests of the pump logic ($RAW). Both are reported in the telemetry.

  Signal processing (as in the DSP paper), per sensor, once per second:
    raw ADC reading (12-read burst average) -> 3-point running median (isolated spikes)
    -> 5-point moving average FIR low-pass (short-term random fluctuations) -> smoothed reading.
  Soil status is DRY or WET only: DRY below START_WATERING_PCT, WET once STOP_WATERING_PCT
  is reached; in between it keeps its last state (hysteresis).

  Serial 115200. Single-letter commands (no newline needed):
    D = save dry-soil references for all three sensors (also pauses AUTO)
    W = save wet-soil references for all three sensors (arms AUTO if calibration is valid)
    A = resume automatic watering (after X, a fault, or calibration)
    X = stop pump, pause AUTO (kept across restarts), clear lockout, end test input
    L = test external LED for 2 seconds
    T = manual 1.5-second pump pulse (requires hardware verification; not in AUTO)
    ? = show commands and the stored calibration
  Line commands (start with $, end with newline):
    $CAL d1 d2 d3 w1 w2 w3   set dry and wet raw references directly (span >= 150 each)
    $RAW r1 r2 r3            TEST INPUT: use these raw values instead of the sensors for 5 minutes
    $RAW OFF                 end the test input

  Telemetry is JSONL: one line starting { each second (protocol v1; v2 adds fields only).
  Other diagnostic messages begin with #EVENT.
*/

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

// -------- HARDWARE: LABELS PRINTED ON ESP32-S3 BOARD --------
const uint8_t SENSOR_PINS[3] = {1, 2, 3};
const uint8_t LED_PIN = 5;
const uint8_t RELAY_PIN = 6;
const uint8_t TFT_RST = 8;
const uint8_t TFT_DC = 9;
const uint8_t TFT_CS = 10;
const uint8_t TFT_MOSI = 11;
const uint8_t TFT_SCK = 12;

// -------- VERIFY THESE SETTINGS BEFORE USING THE PUMP --------
// true: relay input turns relay ON when signal is LOW.
// false: relay input turns relay ON when signal is HIGH.
const bool RELAY_ACTIVE_LOW = true;

// Deliberately FALSE until pump COM/NO wiring and active level are verified.
// Change to true ONLY after the pump physically stops when commanded OFF.
const bool PUMP_HARDWARE_VERIFIED = false;

// Enter AUTO by itself after power-on / wet calibration (still needs PUMP_HARDWARE_VERIFIED).
const bool AUTO_START_AFTER_CALIBRATION = true;

// -------- PROTOTYPE CONTROL SETTINGS --------
// These are percentages relative to dry/wet calibration points,
// NOT laboratory volumetric water content or plant-available water.
const int START_WATERING_PCT = 50;
const int STOP_WATERING_PCT = 70;
const unsigned long SAMPLE_INTERVAL_MS = 1000UL;
const unsigned long PUMP_PULSE_MS = 1500UL;
const unsigned long SOAK_INTERVAL_MS = 20000UL;
const unsigned long BOOT_SETTLE_MS = 5000UL;
const unsigned long TEST_INPUT_MAX_MS = 300000UL;   // $RAW test input ends by itself after 5 min
const uint8_t MAX_PULSES = 3;
const int MIN_CALIBRATION_SPAN_ADC = 150;
const int ADC_BAD_LOW = 10;
const int ADC_BAD_HIGH = 4085;
const uint8_t MEDIAN_POINTS = 3;   // isolated-spike removal
const uint8_t MA_POINTS = 5;       // moving-average FIR low-pass

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);
Preferences prefs;
const char* DRY_KEYS[3] = {"dry0", "dry1", "dry2"};
const char* WET_KEYS[3] = {"wet0", "wet1", "wet2"};

int rawADC[3] = {0, 0, 0};
float filteredADC[3] = {0, 0, 0};
bool filterInitialized = false;

// N-point running median (median of what is there until the window fills).
template <uint8_t N>
struct RunningMedian {
  float buf[N]; uint8_t count = 0, head = 0;
  void reset() { count = 0; head = 0; }
  float update(float x) {
    buf[head] = x; head = (head + 1) % N; if (count < N) count++;
    float t[N];
    for (uint8_t i = 0; i < count; i++) t[i] = buf[i];
    for (uint8_t i = 1; i < count; i++) {          // insertion sort, N is tiny
      float v = t[i]; uint8_t j = i;
      while (j > 0 && t[j - 1] > v) { t[j] = t[j - 1]; j--; }
      t[j] = v;
    }
    return t[count / 2];
  }
};

// M-point moving average FIR (average of what is there until the window fills).
template <uint8_t M>
struct MovingAverage {
  float buf[M]; uint8_t count = 0, head = 0; float sum = 0;
  void reset() { count = 0; head = 0; sum = 0; }
  float update(float x) {
    if (count == M) sum -= buf[head]; else count++;
    buf[head] = x; sum += x; head = (head + 1) % M;
    return sum / (float)count;
  }
};

RunningMedian<MEDIAN_POINTS> spikeFilter[3];
MovingAverage<MA_POINTS> smoothFilter[3];
int dryADC[3] = {-1, -1, -1};
int wetADC[3] = {-1, -1, -1};

bool sensorFault = false;
bool autoEnabled = false;
bool autoPaused = false;       // persistent
bool safetyLockout = false;    // persistent
bool pumpCommandOn = false;
bool manualPulse = false;
bool waterDemand = false;
bool soilDry = false;          // DRY/WET status with hysteresis (valid only when statusKnown)
bool statusKnown = false;
bool ledTestActive = false;
bool ledState = false;
bool testInput = false;
int testRaw[3] = {0, 0, 0};
uint8_t pulsesUsed = 0;
unsigned long settleFromMs = 0;   // no automatic pulse within BOOT_SETTLE_MS of boot or of a new calibration
unsigned long pumpStartMs = 0;
unsigned long lastPumpStopMs = 0;
unsigned long lastSampleMs = 0;
unsigned long ledTestMs = 0;
unsigned long testInputMs = 0;

char lineBuf[64];
uint8_t lineLen = 0;
bool lineMode = false;

char fieldCache[7][48] = {{0}};
uint16_t colorCache[7] = {0};

void setPump(bool on) {
  // NEVER enable pump without hardware verification.
  if (on && !PUMP_HARDWARE_VERIFIED) on = false;
  const bool pinHigh = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(RELAY_PIN, pinHigh ? HIGH : LOW);

  if (on != pumpCommandOn) {
    if (on) {
      pumpStartMs = millis();
      Serial.println("#EVENT pump_command_on");
    } else {
      lastPumpStopMs = millis();
      manualPulse = false;
      Serial.println("#EVENT pump_command_off");
    }
    pumpCommandOn = on;
  }
}

void setPaused(bool p, const char* why) {
  if (p != autoPaused) {
    autoPaused = p;
    prefs.putBool("paused", p);
    Serial.print(p ? "#EVENT auto_paused_" : "#EVENT auto_resumed_");
    Serial.println(why);
  }
}

void setLockout(bool l) {
  if (l != safetyLockout) {
    safetyLockout = l;
    prefs.putBool("lockout", l);
  }
}

int analogAverage(uint8_t pin, int samples) {
  long total = 0;
  for (int i = 0; i < samples; i++) {
    total += analogRead(pin);
    delayMicroseconds(250);
  }
  return (int)(total / samples);
}

void sampleSensors() {
  for (int i = 0; i < 3; i++) {
    rawADC[i] = testInput ? testRaw[i] : analogAverage(SENSOR_PINS[i], 12);
    if (!filterInitialized) { spikeFilter[i].reset(); smoothFilter[i].reset(); }
    filteredADC[i] = smoothFilter[i].update(spikeFilter[i].update(rawADC[i]));
  }
  filterInitialized = true;

  sensorFault = false;
  for (int i = 0; i < 3; i++) {
    if (rawADC[i] <= ADC_BAD_LOW || rawADC[i] >= ADC_BAD_HIGH) {
      sensorFault = true;
    }
  }
}

bool calibrationValid() {
  for (int i = 0; i < 3; i++) {
    if (dryADC[i] < 0 || wetADC[i] < 0 ||
        abs(dryADC[i] - wetADC[i]) < MIN_CALIBRATION_SPAN_ADC)
      return false;
  }
  return true;
}

int sensorPercent(int i) {
  if (!calibrationValid() || sensorFault) return -1;
  float pct = 100.0f * (filteredADC[i] - dryADC[i]) /
              float(wetADC[i] - dryADC[i]);
  return constrain((int)lroundf(pct), 0, 100);
}

int averagePercent() {
  if (!calibrationValid() || sensorFault) return -1;
  return (int)lroundf((sensorPercent(0) + sensorPercent(1) + sensorPercent(2)) / 3.0f);
}

// DRY/WET with hysteresis on the average of the three smoothed readings.
void updateSoilStatus() {
  int pct = averagePercent();
  if (pct < 0) { statusKnown = false; return; }
  if (!statusKnown) { soilDry = pct < START_WATERING_PCT; statusKnown = true; }
  else if (pct < START_WATERING_PCT) soilDry = true;
  else if (pct >= STOP_WATERING_PCT) soilDry = false;
}

bool autoAllowed() {
  return AUTO_START_AFTER_CALIBRATION && PUMP_HARDWARE_VERIFIED && calibrationValid() &&
         !sensorFault && !autoPaused && !safetyLockout;
}

void printCalibration() {
  Serial.print("#EVENT calibration dry=");
  for (int i = 0; i < 3; i++) { if (i) Serial.print(','); Serial.print(dryADC[i]); }
  Serial.print(" wet=");
  for (int i = 0; i < 3; i++) { if (i) Serial.print(','); Serial.print(wetADC[i]); }
  Serial.println(calibrationValid() ? " valid" : " invalid");
}

// Store new references. dry=true/false: one side only; both: $CAL. Arms AUTO when the
// wet side was just set and the calibration is complete (v2 behaviour).
void storeCalibration(const int* dry, const int* wet) {
  for (int i = 0; i < 3; i++) {
    if (dry) { dryADC[i] = dry[i]; prefs.putInt(DRY_KEYS[i], dry[i]); }
    if (wet) { wetADC[i] = wet[i]; prefs.putInt(WET_KEYS[i], wet[i]); }
  }
  pulsesUsed = 0;
  Serial.println(calibrationValid() ? "#EVENT calibration_valid" :
                 "#EVENT calibration_incomplete_or_span_too_small");
  printCalibration();
  if (wet && calibrationValid()) {
    setLockout(false);
    setPaused(false, "after_wet_calibration");
    settleFromMs = millis();          // probes were just handled: let the readings settle first
    autoEnabled = autoAllowed();
    if (autoEnabled) Serial.println("#EVENT auto_enabled");
  } else {
    setPaused(true, "for_calibration");
  }
}

void captureCalibration(bool dry) {
  autoEnabled = false;
  waterDemand = false;
  setPump(false);
  int measured[3];
  for (int i = 0; i < 3; i++) {
    // Capture stable dry/wet reference with repeated samples (always the real sensors).
    long sum = 0;
    for (int j = 0; j < 20; j++) {
      sum += analogRead(SENSOR_PINS[i]);
      delay(5);
    }
    measured[i] = (int)(sum / 20);
    if (measured[i] <= ADC_BAD_LOW || measured[i] >= ADC_BAD_HIGH) {
      Serial.println("#EVENT calibration_rejected_rail_reading");
      return;
    }
  }
  if (!testInput) {
    for (int i = 0; i < 3; i++) {
      rawADC[i] = measured[i];
      spikeFilter[i].reset(); smoothFilter[i].reset();
      filteredADC[i] = smoothFilter[i].update(spikeFilter[i].update(measured[i]));
    }
    filterInitialized = true;
    sensorFault = false;
  }
  Serial.println(dry ? "#EVENT dry_reference_saved" : "#EVENT wet_reference_saved");
  storeCalibration(dry ? measured : nullptr, dry ? nullptr : measured);
}

void controlWatering() {
  const unsigned long now = millis();
  // A sensor fault stops everything and pauses AUTO until an explicit A.
  if (sensorFault) {
    if (autoEnabled) Serial.println("#EVENT auto_stopped_sensor_fault");
    autoEnabled = false;
    waterDemand = false;
    setPump(false);
    if (calibrationValid()) setPaused(true, "sensor_fault");
    return;
  }
  // Without calibration there is no demand; a manual T pulse may still finish.
  if (!calibrationValid()) {
    autoEnabled = false;
    waterDemand = false;
    if (!manualPulse) setPump(false);
    return;
  }

  int pct = averagePercent();
  if (pct < START_WATERING_PCT) waterDemand = true;
  if (pct >= STOP_WATERING_PCT) {
    waterDemand = false;
    pulsesUsed = 0;
    if (!manualPulse) setPump(false);
  }

  if (!autoEnabled && autoAllowed()) {      // e.g. a fault cleared without a pause being stored
    autoEnabled = true;
    Serial.println("#EVENT auto_enabled");
  }
  if (!autoEnabled || safetyLockout || !waterDemand || pumpCommandOn) return;
  if (now - settleFromMs < BOOT_SETTLE_MS) return;

  bool soaked = pulsesUsed == 0 || (now - lastPumpStopMs >= SOAK_INTERVAL_MS);
  if (!soaked) return;

  // Still dry after the last allowed pulse has soaked in: stop and wait for a person.
  if (pulsesUsed >= MAX_PULSES) {
    setLockout(true);
    autoEnabled = false;
    setPump(false);
    Serial.println("#EVENT safety_lockout_max_pulses");
    return;
  }
  setPump(true);
  if (pumpCommandOn) pulsesUsed++;
}

void updateLED() {
  bool shouldLight = ledTestActive ||
    (sensorFault && ((millis() / 400UL) % 2 == 0)) ||
    (calibrationValid() && waterDemand);
  ledState = shouldLight;
  digitalWrite(LED_PIN, shouldLight ? HIGH : LOW);
}

void drawValue(int id, int x, int y, int width, int height,
               int fontSize, const char* text, uint16_t color) {
  if (strcmp(fieldCache[id], text) == 0 && colorCache[id] == color)
    return;
  tft.fillRect(x, y, width, height, ILI9341_BLACK);
  tft.setCursor(x, y);
  tft.setTextSize(fontSize);
  tft.setTextColor(color);
  tft.print(text);
  strncpy(fieldCache[id], text, sizeof(fieldCache[id]) - 1);
  fieldCache[id][sizeof(fieldCache[id]) - 1] = '\0';
  colorCache[id] = color;
}

void initDisplay() {
  tft.begin();
  tft.setRotation(1); // 320 x 240 landscape
  tft.fillScreen(ILI9341_BLACK);  // ONCE only at startup
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(8, 8);
  tft.print("SMART IRRIGATION");
  tft.drawFastHLine(8, 31, 304, ILI9341_DARKCYAN);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(8, 43);  tft.print("S1:");
  tft.setCursor(8, 73);  tft.print("S2:");
  tft.setCursor(8, 103); tft.print("S3:");
  tft.setCursor(8, 135); tft.print("AVG:");
  tft.setCursor(8, 169); tft.print("PUMP:");
  tft.setTextSize(1);
  tft.setCursor(8, 199); tft.print("MODE:");
  tft.setCursor(8, 219); tft.print("INFO:");
}

void updateDisplay() {
  char buf[48];
  for (int i = 0; i < 3; i++) {
    int pct = sensorPercent(i);
    if (pct < 0) snprintf(buf, sizeof(buf), "RAW %4d", rawADC[i]);
    else snprintf(buf, sizeof(buf), "%3d%%  raw %4d", pct, rawADC[i]);
    drawValue(i, 60, 43 + i * 30, 250, 17, 2, buf,
              testInput ? ILI9341_YELLOW : ILI9341_WHITE);
  }
  if (sensorFault) {
    snprintf(buf, sizeof(buf), "FAULT");
    drawValue(3, 72, 135, 238, 17, 2, buf, ILI9341_RED);
  } else if (!calibrationValid()) {
    snprintf(buf, sizeof(buf), "CALIBRATE");
    drawValue(3, 72, 135, 238, 17, 2, buf, ILI9341_YELLOW);
  } else {
    snprintf(buf, sizeof(buf), "%d%% %s", averagePercent(), soilDry ? "DRY" : "WET");
    drawValue(3, 72, 135, 238, 17, 2, buf,
              waterDemand ? ILI9341_RED : ILI9341_GREEN);
  }
  drawValue(4, 95, 169, 215, 17, 2,
            pumpCommandOn ? "ON" : "OFF",
            pumpCommandOn ? ILI9341_GREEN : ILI9341_RED);
  drawValue(5, 65, 199, 245, 10, 1,
            safetyLockout ? "LOCKOUT" : (autoEnabled ? "AUTO" : (autoPaused ? "PAUSED (send A)" : "MANUAL")),
            safetyLockout ? ILI9341_RED : ILI9341_CYAN);
  if (testInput) snprintf(buf, sizeof(buf), "TEST INPUT ($RAW)  PULSES %d/%d", pulsesUsed, MAX_PULSES);
  else if (!PUMP_HARDWARE_VERIFIED) snprintf(buf, sizeof(buf), "PUMP WIRING NOT VERIFIED");
  else if (sensorFault) snprintf(buf, sizeof(buf), "CHECK SENSOR POWER/AO");
  else if (!calibrationValid()) snprintf(buf, sizeof(buf), "SEND D THEN W");
  else if (safetyLockout) snprintf(buf, sizeof(buf), "MAX PULSES; SEND X");
  else snprintf(buf, sizeof(buf), "%s  PULSES %d/%d", soilDry ? "DRY" : "WET", pulsesUsed, MAX_PULSES);
  drawValue(6, 65, 219, 245, 10, 1, buf, ILI9341_YELLOW);
}

void printRefs(const int* r) {
  Serial.print('[');
  for (int i = 0; i < 3; i++) {
    if (i) Serial.print(',');
    if (r[i] < 0) Serial.print("null");
    else Serial.print(r[i]);
  }
  Serial.print(']');
}

void emitTelemetry() {
  // JSON Lines (JSONL) for the DSP / desktop / dashboard software.
  // Uptime ms is not a real-world timestamp.
  Serial.print("{\"type\":\"telemetry\",\"v\":1,\"ms\":");
  Serial.print(millis());
  Serial.print(",\"raw\":[");
  for (int i = 0; i < 3; i++) {
    if (i) Serial.print(',');
    Serial.print(rawADC[i]);
  }
  Serial.print("],\"filtered_raw\":[");
  for (int i = 0; i < 3; i++) {
    if (i) Serial.print(',');
    Serial.print((int)lroundf(filteredADC[i]));
  }
  Serial.print("],\"moisture_pct\":[");
  for (int i = 0; i < 3; i++) {
    if (i) Serial.print(',');
    int pct = sensorPercent(i);
    if (pct < 0) Serial.print("null");
    else Serial.print(pct);
  }
  Serial.print("],\"average_pct\":");
  int avg = averagePercent();
  if (avg < 0) Serial.print("null");
  else Serial.print(avg);
  Serial.print(",\"calibrated\":"); Serial.print(calibrationValid() ? "true" : "false");
  Serial.print(",\"sensor_fault\":"); Serial.print(sensorFault ? "true" : "false");
  Serial.print(",\"water_needed\":"); Serial.print(waterDemand ? "true" : "false");
  Serial.print(",\"pump_commanded\":"); Serial.print(pumpCommandOn ? "true" : "false");
  Serial.print(",\"led_on\":"); Serial.print(ledState ? "true" : "false");
  Serial.print(",\"auto_mode\":"); Serial.print(autoEnabled ? "true" : "false");
  Serial.print(",\"lockout\":"); Serial.print(safetyLockout ? "true" : "false");
  Serial.print(",\"pulses\":"); Serial.print(pulsesUsed);
  Serial.print(",\"hardware_verified\":"); Serial.print(PUMP_HARDWARE_VERIFIED ? "true" : "false");
  Serial.print(",\"paused\":"); Serial.print(autoPaused ? "true" : "false");
  Serial.print(",\"test_input\":"); Serial.print(testInput ? "true" : "false");
  Serial.print(",\"dry_ref\":"); printRefs(dryADC);
  Serial.print(",\"wet_ref\":"); printRefs(wetADC);
  if (statusKnown) { Serial.print(",\"status\":"); Serial.print(soilDry ? "\"DRY\"" : "\"WET\""); }
  Serial.println("}");
}

void showHelp() {
  Serial.println("#EVENT commands D=dry W=wet A=auto X=stop/pause L=LED T=pump ?=help | $CAL d1 d2 d3 w1 w2 w3 | $RAW r1 r2 r3 | $RAW OFF");
  printCalibration();
}

void endTestInput(const char* why) {
  if (!testInput) return;
  testInput = false;
  filterInitialized = false;   // restart the filters from the real sensors
  Serial.print("#EVENT test_input_off_");
  Serial.println(why);
}

// Parses exactly n integers in 0..4095 from s. Returns false on anything else.
bool parseInts(const char* s, int* out, int n) {
  for (int i = 0; i < n; i++) {
    char* end;
    long v = strtol(s, &end, 10);
    if (end == s || v < 0 || v > 4095) return false;
    out[i] = (int)v;
    s = end;
  }
  while (*s == ' ') s++;
  return *s == '\0';
}

void handleLine(const char* line) {
  if (strncmp(line, "CAL ", 4) == 0) {
    int v[6];
    if (!parseInts(line + 4, v, 6)) { Serial.println("#EVENT cal_rejected_need_6_values_0_4095"); return; }
    for (int i = 0; i < 3; i++) {
      if (v[i] <= ADC_BAD_LOW || v[i] >= ADC_BAD_HIGH || v[i + 3] <= ADC_BAD_LOW || v[i + 3] >= ADC_BAD_HIGH ||
          abs(v[i] - v[i + 3]) < MIN_CALIBRATION_SPAN_ADC) {
        Serial.println("#EVENT cal_rejected_rail_value_or_span_below_150");
        return;
      }
    }
    autoEnabled = false;
    waterDemand = false;
    setPump(false);
    Serial.println("#EVENT calibration_set_from_values");
    storeCalibration(v, v + 3);
  } else if (strcmp(line, "RAW OFF") == 0) {
    endTestInput("by_command");
  } else if (strncmp(line, "RAW ", 4) == 0) {
    int v[3];
    if (!parseInts(line + 4, v, 3)) { Serial.println("#EVENT raw_rejected_need_3_values_0_4095"); return; }
    for (int i = 0; i < 3; i++) testRaw[i] = v[i];
    if (!testInput) filterInitialized = false;   // start the filters at the test values
    testInput = true;
    testInputMs = millis();
    Serial.println("#EVENT test_input_on");
  } else {
    Serial.println("#EVENT unknown_line_command");
  }
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (lineMode) {
      if (c == '\n' || c == '\r') {
        lineBuf[lineLen] = '\0';
        lineMode = false;
        for (char* p = lineBuf; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        handleLine(lineBuf);
      } else if (lineLen < sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
      } else {
        lineMode = false;
        Serial.println("#EVENT line_command_too_long");
      }
      continue;
    }
    if (c == '$') { lineMode = true; lineLen = 0; }
    else if (c == 'D' || c == 'd') captureCalibration(true);
    else if (c == 'W' || c == 'w') captureCalibration(false);
    else if (c == 'L' || c == 'l') {
      ledTestActive = true;
      ledTestMs = millis();
      Serial.println("#EVENT led_test_2s");
    } else if (c == 'X' || c == 'x') {
      setPump(false);
      autoEnabled = false;
      setLockout(false);
      setPaused(true, "by_X");
      waterDemand = false;
      pulsesUsed = 0;
      endTestInput("by_X");
      Serial.println("#EVENT stopped_and_lockout_cleared");
    } else if (c == 'A' || c == 'a') {
      if (!PUMP_HARDWARE_VERIFIED) Serial.println("#EVENT auto_denied_check_NO_COM_and_trigger_polarity");
      else if (safetyLockout) Serial.println("#EVENT auto_denied_lockout_send_X_then_review");
      else if (!calibrationValid()) Serial.println("#EVENT auto_denied_calibrate_D_then_W");
      else if (sensorFault) Serial.println("#EVENT auto_denied_sensor_fault");
      else if (pumpCommandOn) Serial.println("#EVENT auto_denied_pump_busy");
      else {
        setPaused(false, "by_A");
        autoEnabled = true;
        waterDemand = false;  // Only start a new cycle below START threshold.
        pulsesUsed = 0;
        Serial.println("#EVENT auto_enabled");
      }
    } else if (c == 'T' || c == 't') {
      if (!PUMP_HARDWARE_VERIFIED || sensorFault || autoEnabled || safetyLockout || pumpCommandOn)
        Serial.println("#EVENT manual_pump_test_denied");
      else {
        manualPulse = true;
        setPump(true);
        Serial.println("#EVENT manual_pump_test_1p5s");
      }
    } else if (c == '?') showHelp();
  }
}

void setup() {
  Serial.begin(115200);
  // De-energize the relay as early as software permits.
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  analogReadResolution(12);
  for (int i = 0; i < 3; i++) {
    pinMode(SENSOR_PINS[i], INPUT);
    analogSetPinAttenuation(SENSOR_PINS[i], ADC_11db);
  }

  prefs.begin("soilcfg", false);
  for (int i = 0; i < 3; i++) {
    dryADC[i] = prefs.getInt(DRY_KEYS[i], -1);
    wetADC[i] = prefs.getInt(WET_KEYS[i], -1);
  }
  autoPaused = prefs.getBool("paused", false);
  safetyLockout = prefs.getBool("lockout", false);

  initDisplay();
  // LED wiring self-test, regardless of moisture calibration.
  digitalWrite(LED_PIN, HIGH);
  delay(2000);
  digitalWrite(LED_PIN, LOW);

  settleFromMs = millis();
  sampleSensors();
  updateSoilStatus();
  autoEnabled = autoAllowed();
  updateDisplay();
  Serial.println("#EVENT esp32_s3_irrigation_v2_boot");
  Serial.println("#EVENT led_startup_test_complete");
  if (!PUMP_HARDWARE_VERIFIED) Serial.println("#EVENT PUMP_DISABLED_UNTIL_HARDWARE_VERIFIED");
  if (!calibrationValid()) Serial.println("#EVENT calibration_needed_send_D_and_W");
  if (autoPaused) Serial.println("#EVENT auto_paused_send_A_to_resume");
  if (safetyLockout) Serial.println("#EVENT lockout_active_inspect_then_send_X");
  if (autoEnabled) Serial.println("#EVENT auto_enabled_at_boot");
  showHelp();
  lastSampleMs = millis();
}

void loop() {
  handleSerial();
  unsigned long now = millis();

  // Time-based pump cutoff independent of sensor sampling.
  if (pumpCommandOn && (now - pumpStartMs >= PUMP_PULSE_MS)) setPump(false);
  if (ledTestActive && (now - ledTestMs >= 2000UL)) ledTestActive = false;
  if (testInput && (now - testInputMs >= TEST_INPUT_MAX_MS)) endTestInput("timeout");

  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;
    sampleSensors();
    updateSoilStatus();
    controlWatering();
    updateLED();
    updateDisplay();
    emitTelemetry();
  } else {
    updateLED();
  }
}

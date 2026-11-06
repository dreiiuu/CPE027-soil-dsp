/*
  SMART SOIL IRRIGATION - ESP32-S3 N16R8
  Version 2.8 - built-in calibration (no D/W needed), 60/98 % thresholds, a faulty sensor is only a warning

  HARDWARE PINS
  Sensors: GPIO 1, 2, 3
  LED: GPIO 6
  Relay IN: GPIO 5
  No TFT display: the PC dashboard (USB serial) shows the system.

  IMPORTANT:
  - Pump positive connects to relay NO.
  - Relay COM connects to external pump adapter +5V.
  - Pump negative connects to external adapter GND.
  - Relay GND and ESP32 GND must be common.
  - Never connect the pump directly to an ESP32 GPIO.
  - NEVER connect external 5V directly to ESP32 3V3.
  - Keep the pump physically disconnected during R relay diagnostics.

  FAST RESPONSE UPDATE (v2.3)
  - Moisture control samples every 100 ms (10 times per second).
  - The moving-average filter is reduced from 5 to 3 samples to reduce lag.
  - When the filtered three-sensor average reaches 98% (STOP_WATERING_PCT), the pump is switched OFF
    during that control pass, without waiting for the pulse or soak timer.
  - Serial telemetry remains at 1-second intervals.

  v2.7: a faulty or uncalibrated sensor is skipped with a warning and the others keep working:
  no pause, no stop. Calibration stores the good sensors and skips a bad one. Only when no
  deciding sensor is usable is the pump held off (and it continues by itself once one is).

  v2.6: a sensor outside SENSOR_USED (S2) is shown with its own calibrated percentage, but it is
  never counted in the average, the fault check or the pump decision.

  v2.5: SENSOR_USED picks the sensors that are connected (S2 is disconnected). Unused sensors
  are ignored for faults, calibration checks and the average. A second D/W tap within 2 s is
  ignored, and a refused calibration names the sensor that caused it.

  v2.4: the TFT display was removed from the hardware. Its code and the Adafruit
  libraries are gone; GPIO 8-12 are free.
  - Actual pump/relay stopping time must be measured.

  COMMANDS
  D = Capture dry calibration
  W = Capture wet calibration
  A = Resume automatic watering
  X = Stop and pause
  L = LED test
  R = Relay-only diagnostic pulse
  T = Manual pump pulse
  ? = Help

  LINE COMMANDS
  $CAL d1 d2 d3 w1 w2 w3
  $RAW r1 r2 r3
  $RAW OFF
*/

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <Preferences.h>

// -------- HARDWARE PINS --------
const uint8_t SENSOR_PINS[3] = {1, 2, 3};

// Sensors that decide the watering. false (S2) = shown only: ignored for faults, the
// calibration check and the average; its own percentage is still sent to the dashboard.
const bool SENSOR_USED[3] = {true, false, true};

const uint8_t LED_PIN = 6;
const uint8_t RELAY_PIN = 5;

// -------- RELAY CONFIGURATION --------
// true = relay activates when GPIO is LOW
// false = relay activates when GPIO is HIGH
const bool RELAY_ACTIVE_LOW = true;

// Hardware has been verified.
const bool PUMP_HARDWARE_VERIFIED = true;

const bool AUTO_START_AFTER_CALIBRATION = true;

// -------- BUILT-IN CALIBRATION --------
// Raw ADC references of this board's probes, from the 30 Oct 2026 session log (dry = probe in dry
// air/soil, wet = probe in water). Used whenever a sensor has no valid stored calibration, so the
// system works straight after upload without pressing D/W. D/W or $CAL still override them.
const int DEFAULT_DRY_ADC[3] = {1504, 1644, 1943};
const int DEFAULT_WET_ADC[3] = {1096, 624, 1187};

// -------- IRRIGATION SETTINGS --------
const int START_WATERING_PCT = 60;   // DRY below 60 % -> water
const int STOP_WATERING_PCT = 98;    // WET from 98 % -> stop

const unsigned long SAMPLE_INTERVAL_MS = 100UL;
const unsigned long UI_UPDATE_INTERVAL_MS = 1000UL;
const unsigned long PUMP_PULSE_MS = 1500UL;
const unsigned long SOAK_INTERVAL_MS = 5000UL;
const unsigned long BOOT_SETTLE_MS = 5000UL;
const unsigned long TEST_INPUT_MAX_MS = 300000UL;

const uint8_t MAX_PULSES = 3;
const int MIN_CALIBRATION_SPAN_ADC = 150;

const int ADC_BAD_LOW = 10;
const int ADC_BAD_HIGH = 4085;
const unsigned long CAL_TAP_GUARD_MS = 2000UL;   // ignore a repeated D/W tap within this time

const uint8_t MEDIAN_POINTS = 3;
const uint8_t MA_POINTS = 3;

// -------- OBJECTS --------
Preferences prefs;

const char* DRY_KEYS[3] = {"dry0", "dry1", "dry2"};
const char* WET_KEYS[3] = {"wet0", "wet1", "wet2"};

// -------- SENSOR VARIABLES --------
int rawADC[3] = {0, 0, 0};
float filteredADC[3] = {0, 0, 0};

bool filterInitialized = false;

// -------- FILTERS --------
template <uint8_t N>
struct RunningMedian {
  float buf[N];
  uint8_t count = 0;
  uint8_t head = 0;

  void reset() {
    count = 0;
    head = 0;
  }

  float update(float x) {
    buf[head] = x;
    head = (head + 1) % N;

    if (count < N) count++;

    float t[N];

    for (uint8_t i = 0; i < count; i++) {
      t[i] = buf[i];
    }

    for (uint8_t i = 1; i < count; i++) {
      float v = t[i];
      uint8_t j = i;

      while (j > 0 && t[j - 1] > v) {
        t[j] = t[j - 1];
        j--;
      }

      t[j] = v;
    }

    return t[count / 2];
  }
};

template <uint8_t M>
struct MovingAverage {
  float buf[M];
  uint8_t count = 0;
  uint8_t head = 0;
  float sum = 0;

  void reset() {
    count = 0;
    head = 0;
    sum = 0;
  }

  float update(float x) {
    if (count == M) {
      sum -= buf[head];
    } else {
      count++;
    }

    buf[head] = x;
    sum += x;
    head = (head + 1) % M;

    return sum / (float)count;
  }
};

RunningMedian<MEDIAN_POINTS> spikeFilter[3];
MovingAverage<MA_POINTS> smoothFilter[3];

// -------- CALIBRATION --------
int dryADC[3] = {-1, -1, -1};
int wetADC[3] = {-1, -1, -1};

// -------- SYSTEM STATES --------
bool sensorFault = false;
bool autoEnabled = false;
bool autoPaused = false;
bool safetyLockout = false;

bool pumpCommandOn = false;
bool manualPulse = false;
bool waterDemand = false;

bool soilDry = false;
bool statusKnown = false;

bool ledTestActive = false;
bool ledState = false;

bool testInput = false;

bool noUsableSensor = false;

bool calTapped = false;
unsigned long lastCalTapMs = 0;

int testRaw[3] = {0, 0, 0};

uint8_t pulsesUsed = 0;

// -------- RELAY DIAGNOSTIC VARIABLES --------
bool relayTestActive = false;
bool relayTestCommandOn = false;

unsigned long relayTestStartMs = 0;

// -------- TIMERS --------
unsigned long settleFromMs = 0;
unsigned long pumpStartMs = 0;
unsigned long lastPumpStopMs = 0;
unsigned long lastSampleMs = 0;
unsigned long lastUiUpdateMs = 0;
unsigned long ledTestMs = 0;
unsigned long testInputMs = 0;

// -------- SERIAL LINE BUFFER --------
char lineBuf[64];
uint8_t lineLen = 0;
bool lineMode = false;

// =====================================================
// RELAY DIAGNOSTIC
// =====================================================

void setRelayDiagnosticOutput(bool on) {
  const bool pinHigh = RELAY_ACTIVE_LOW ? !on : on;

  digitalWrite(RELAY_PIN, pinHigh ? HIGH : LOW);

  relayTestCommandOn = on;
}

void stopRelayDiagnostic() {
  if (relayTestActive || relayTestCommandOn) {
    setRelayDiagnosticOutput(false);

    relayTestActive = false;

    Serial.println(
      "#EVENT relay_diagnostic_finished_output_off"
    );
  }
}

// =====================================================
// PUMP CONTROL
// =====================================================

void setPump(bool on) {

  if (on && !PUMP_HARDWARE_VERIFIED) {
    on = false;
  }

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

// =====================================================
// PAUSE AND LOCKOUT
// =====================================================

void setPaused(bool p, const char* why) {

  if (p != autoPaused) {

    autoPaused = p;

    prefs.putBool("paused", p);

    Serial.print(
      p ? "#EVENT auto_paused_" : "#EVENT auto_resumed_"
    );

    Serial.println(why);
  }
}

void setLockout(bool l) {

  if (l != safetyLockout) {

    safetyLockout = l;

    prefs.putBool("lockout", l);
  }
}

// =====================================================
// SENSOR READING
// =====================================================

bool sensorBad(int i) {
  return rawADC[i] <= ADC_BAD_LOW || rawADC[i] >= ADC_BAD_HIGH;
}

bool refsValid(int i) {
  return dryADC[i] >= 0 && wetADC[i] >= 0 &&
         abs(dryADC[i] - wetADC[i]) >= MIN_CALIBRATION_SPAN_ADC;
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

    rawADC[i] = testInput
      ? testRaw[i]
      : analogAverage(SENSOR_PINS[i], 12);

    if (!filterInitialized) {
      spikeFilter[i].reset();
      smoothFilter[i].reset();
    }

    filteredADC[i] =
      smoothFilter[i].update(
        spikeFilter[i].update(rawADC[i])
      );
  }

  filterInitialized = true;

  // sensorFault = at least one deciding sensor reads 0 or 4095 (a warning only)
  sensorFault = false;

  for (int i = 0; i < 3; i++) {

    if (SENSOR_USED[i] && sensorBad(i)) {
      sensorFault = true;
    }
  }
}

// =====================================================
// CALIBRATION
// =====================================================

// Calibrated = at least one deciding sensor has valid dry/wet references.
bool calibrationValid() {

  for (int i = 0; i < 3; i++) {
    if (SENSOR_USED[i] && refsValid(i)) return true;
  }

  return false;
}

// Each sensor on its own: -1 (skipped) when it has no valid references or reads 0 / 4095.
// A sensor outside SENSOR_USED (S2) is shown but never counts in the average.
int sensorPercent(int i) {

  if (!refsValid(i) || sensorBad(i)) {
    return -1;
  }

  float pct =
    100.0f *
    (filteredADC[i] - dryADC[i]) /
    float(wetADC[i] - dryADC[i]);

  return constrain((int)lroundf(pct), 0, 100);
}

// Average of the deciding sensors that are usable right now; -1 if none is.
int averagePercent() {

  int sum = 0;
  int used = 0;

  for (int i = 0; i < 3; i++) {
    int p = SENSOR_USED[i] ? sensorPercent(i) : -1;
    if (p >= 0) {
      sum += p;
      used++;
    }
  }

  if (used == 0) {
    return -1;
  }

  return (int)lroundf(sum / (float)used);
}

void updateSoilStatus() {

  int pct = averagePercent();

  if (pct < 0) {
    statusKnown = false;
    return;
  }

  if (!statusKnown) {

    soilDry = pct < START_WATERING_PCT;

    statusKnown = true;

  } else if (pct < START_WATERING_PCT) {

    soilDry = true;

  } else if (pct >= STOP_WATERING_PCT) {

    soilDry = false;
  }
}

bool autoAllowed() {

  return
    AUTO_START_AFTER_CALIBRATION &&
    PUMP_HARDWARE_VERIFIED &&
    calibrationValid() &&
    !autoPaused &&
    !safetyLockout;
}

// =====================================================
// CALIBRATION STORAGE
// =====================================================

void printCalibration() {

  Serial.print("#EVENT calibration dry=");

  for (int i = 0; i < 3; i++) {

    if (i) Serial.print(',');

    Serial.print(dryADC[i]);
  }

  Serial.print(" wet=");

  for (int i = 0; i < 3; i++) {

    if (i) Serial.print(',');

    Serial.print(wetADC[i]);
  }

  Serial.println(
    calibrationValid() ? " valid" : " invalid"
  );
}

void storeCalibration(
  const int* dry,
  const int* wet
) {

  for (int i = 0; i < 3; i++) {

    if (dry) {
      dryADC[i] = dry[i];
      prefs.putInt(DRY_KEYS[i], dry[i]);
    }

    if (wet) {
      wetADC[i] = wet[i];
      prefs.putInt(WET_KEYS[i], wet[i]);
    }
  }

  pulsesUsed = 0;

  Serial.println(
    calibrationValid()
      ? "#EVENT calibration_valid"
      : "#EVENT calibration_incomplete_or_span_too_small"
  );

  printCalibration();

  if (wet && calibrationValid()) {

    setLockout(false);

    setPaused(false, "after_wet_calibration");

    settleFromMs = millis();

    autoEnabled = autoAllowed();

    if (autoEnabled) {
      Serial.println("#EVENT auto_enabled");
    }

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

    long sum = 0;

    for (int j = 0; j < 20; j++) {

      sum += analogRead(SENSOR_PINS[i]);

      delay(5);
    }

    measured[i] = (int)(sum / 20);
  }

  // A sensor at 0 / 4095 is skipped (keeps its old reference); the others are stored.
  int good = 0;

  for (int i = 0; i < 3; i++) {

    if (measured[i] <= ADC_BAD_LOW || measured[i] >= ADC_BAD_HIGH) {

      Serial.print("#EVENT calibration_skipped_S");
      Serial.print(i + 1);
      Serial.println("_reads_0_or_4095_check_wiring");

      measured[i] = dry ? dryADC[i] : wetADC[i];

    } else if (SENSOR_USED[i]) {

      good++;
    }
  }

  if (good == 0) {

    Serial.println("#EVENT calibration_rejected_no_usable_sensor");

    return;
  }

  if (!testInput) {

    for (int i = 0; i < 3; i++) {

      rawADC[i] = measured[i];

      spikeFilter[i].reset();
      smoothFilter[i].reset();

      filteredADC[i] =
        smoothFilter[i].update(
          spikeFilter[i].update(measured[i])
        );
    }

    filterInitialized = true;
  }

  Serial.println(
    dry
      ? "#EVENT dry_reference_saved"
      : "#EVENT wet_reference_saved"
  );

  storeCalibration(
    dry ? measured : nullptr,
    dry ? nullptr : measured
  );
}

// =====================================================
// AUTOMATIC WATERING
// =====================================================

void controlWatering() {

  const unsigned long now = millis();

  if (relayTestActive) {
    return;
  }

  if (!calibrationValid()) {

    autoEnabled = false;
    waterDemand = false;

    if (!manualPulse) {
      setPump(false);
    }

    return;
  }

  int pct = averagePercent();

  // No usable deciding sensor right now: no decision, pump off, nothing paused.
  // It carries on by itself as soon as one sensor reads normally again.
  if (pct < 0) {

    if (!noUsableSensor) {
      Serial.println("#EVENT no_usable_sensor_pump_held_off");
      noUsableSensor = true;
    }

    waterDemand = false;

    if (!manualPulse) {
      setPump(false);
    }

    return;
  }

  if (noUsableSensor) {
    Serial.println("#EVENT sensor_usable_again");
    noUsableSensor = false;
  }

  if (pct < START_WATERING_PCT) {
    waterDemand = true;
  }

  if (pct >= STOP_WATERING_PCT) {

    waterDemand = false;

    pulsesUsed = 0;

    if (!manualPulse) {
      setPump(false);
    }
  }

  if (!autoEnabled && autoAllowed()) {

    autoEnabled = true;

    Serial.println("#EVENT auto_enabled");
  }

  if (
    !autoEnabled ||
    safetyLockout ||
    !waterDemand ||
    pumpCommandOn
  ) {
    return;
  }

  if (now - settleFromMs < BOOT_SETTLE_MS) {
    return;
  }

  bool soaked =
    pulsesUsed == 0 ||
    (now - lastPumpStopMs >= SOAK_INTERVAL_MS);

  if (!soaked) {
    return;
  }

  if (pulsesUsed >= MAX_PULSES) {

    setLockout(true);

    autoEnabled = false;

    setPump(false);

    Serial.println(
      "#EVENT safety_lockout_max_pulses"
    );

    return;
  }

  setPump(true);

  if (pumpCommandOn) {
    pulsesUsed++;
  }
}

// =====================================================
// LED
// =====================================================

void updateLED() {

  bool shouldLight =
    ledTestActive ||
    (
      calibrationValid() && averagePercent() < 0 &&
      ((millis() / 400UL) % 2 == 0)
    ) ||
    (
      calibrationValid() &&
      waterDemand
    );

  ledState = shouldLight;

  digitalWrite(
    LED_PIN,
    shouldLight ? HIGH : LOW
  );
}

// =====================================================
// TELEMETRY
// =====================================================

void printRefs(const int* r) {

  Serial.print('[');

  for (int i = 0; i < 3; i++) {

    if (i) {
      Serial.print(',');
    }

    if (r[i] < 0) {
      Serial.print("null");
    } else {
      Serial.print(r[i]);
    }
  }

  Serial.print(']');
}

void emitTelemetry() {

  Serial.print(
    "{\"type\":\"telemetry\",\"v\":1,\"ms\":"
  );

  Serial.print(millis());

  Serial.print(",\"raw\":[");

  for (int i = 0; i < 3; i++) {

    if (i) Serial.print(',');

    Serial.print(rawADC[i]);
  }

  Serial.print("],\"filtered_raw\":[");

  for (int i = 0; i < 3; i++) {

    if (i) Serial.print(',');

    Serial.print(
      (int)lroundf(filteredADC[i])
    );
  }

  Serial.print("],\"moisture_pct\":[");

  for (int i = 0; i < 3; i++) {

    if (i) Serial.print(',');

    int pct = sensorPercent(i);

    if (pct < 0) {
      Serial.print("null");
    } else {
      Serial.print(pct);
    }
  }

  Serial.print("],\"average_pct\":");

  int avg = averagePercent();

  if (avg < 0) {
    Serial.print("null");
  } else {
    Serial.print(avg);
  }

  Serial.print(",\"calibrated\":");
  Serial.print(calibrationValid() ? "true" : "false");

  Serial.print(",\"sensor_fault\":");
  Serial.print(sensorFault ? "true" : "false");

  Serial.print(",\"water_needed\":");
  Serial.print(waterDemand ? "true" : "false");

  Serial.print(",\"pump_commanded\":");
  Serial.print(pumpCommandOn ? "true" : "false");

  Serial.print(",\"led_on\":");
  Serial.print(ledState ? "true" : "false");

  Serial.print(",\"auto_mode\":");
  Serial.print(autoEnabled ? "true" : "false");

  Serial.print(",\"lockout\":");
  Serial.print(safetyLockout ? "true" : "false");

  Serial.print(",\"pulses\":");
  Serial.print(pulsesUsed);

  Serial.print(",\"hardware_verified\":");
  Serial.print(
    PUMP_HARDWARE_VERIFIED ? "true" : "false"
  );

  Serial.print(",\"relay_test_active\":");
  Serial.print(relayTestActive ? "true" : "false");

  Serial.print(",\"relay_output_commanded\":");
  Serial.print(relayTestCommandOn ? "true" : "false");

  Serial.print(",\"paused\":");
  Serial.print(autoPaused ? "true" : "false");

  Serial.print(",\"test_input\":");
  Serial.print(testInput ? "true" : "false");

  Serial.print(",\"dry_ref\":");
  printRefs(dryADC);

  Serial.print(",\"wet_ref\":");
  printRefs(wetADC);

  Serial.print(",\"sensors_used\":[");

  for (int i = 0; i < 3; i++) {
    if (i) Serial.print(',');
    Serial.print(SENSOR_USED[i] ? "true" : "false");
  }

  Serial.print(']');

  Serial.print(",\"sensor_ok\":[");

  for (int i = 0; i < 3; i++) {
    if (i) Serial.print(',');
    Serial.print(sensorPercent(i) >= 0 ? "true" : "false");
  }

  Serial.print(']');

  if (statusKnown) {

    Serial.print(",\"status\":");

    Serial.print(
      soilDry ? "\"DRY\"" : "\"WET\""
    );
  }

  Serial.println("}");
}

// =====================================================
// SERIAL COMMANDS
// =====================================================

void showHelp() {

  Serial.println(
    "#EVENT commands D=dry W=wet A=auto X=stop/pause "
    "L=LED R=relay_diagnostic T=pump ?=help | "
    "$CAL d1 d2 d3 w1 w2 w3 | "
    "$RAW r1 r2 r3 | $RAW OFF"
  );

  Serial.println(
    "#EVENT R_REQUIRES_PUMP_POSITIVE_WIRE_DISCONNECTED_FROM_RELAY_NO"
  );

  printCalibration();
}

void endTestInput(const char* why) {

  if (!testInput) {
    return;
  }

  testInput = false;

  filterInitialized = false;

  Serial.print("#EVENT test_input_off_");
  Serial.println(why);
}

bool parseInts(
  const char* s,
  int* out,
  int n
) {

  for (int i = 0; i < n; i++) {

    char* end;

    long v = strtol(s, &end, 10);

    if (
      end == s ||
      v < 0 ||
      v > 4095
    ) {
      return false;
    }

    out[i] = (int)v;

    s = end;
  }

  while (*s == ' ') {
    s++;
  }

  return *s == '\0';
}

void handleLine(const char* line) {

  if (strncmp(line, "CAL ", 4) == 0) {

    int v[6];

    if (!parseInts(line + 4, v, 6)) {

      Serial.println(
        "#EVENT cal_rejected_need_6_values_0_4095"
      );

      return;
    }

    // A sensor whose pair is at a rail or closer than 150 counts is skipped (keeps its
    // old references); the others are stored. Rejected only if no deciding sensor is valid.
    int good = 0;

    for (int i = 0; i < 3; i++) {

      if (
        v[i] <= ADC_BAD_LOW ||
        v[i] >= ADC_BAD_HIGH ||
        v[i + 3] <= ADC_BAD_LOW ||
        v[i + 3] >= ADC_BAD_HIGH ||
        abs(v[i] - v[i + 3]) <
          MIN_CALIBRATION_SPAN_ADC
      ) {

        Serial.print("#EVENT cal_skipped_S");
        Serial.print(i + 1);
        Serial.println("_rail_value_or_span_below_150");

        v[i] = dryADC[i];
        v[i + 3] = wetADC[i];

      } else if (SENSOR_USED[i]) {

        good++;
      }
    }

    if (good == 0) {

      Serial.println(
        "#EVENT cal_rejected_no_usable_sensor"
      );

      return;
    }

    autoEnabled = false;
    waterDemand = false;

    setPump(false);

    Serial.println(
      "#EVENT calibration_set_from_values"
    );

    storeCalibration(v, v + 3);

  } else if (strcmp(line, "RAW OFF") == 0) {

    endTestInput("by_command");

  } else if (strncmp(line, "RAW ", 4) == 0) {

    int v[3];

    if (!parseInts(line + 4, v, 3)) {

      Serial.println(
        "#EVENT raw_rejected_need_3_values_0_4095"
      );

      return;
    }

    for (int i = 0; i < 3; i++) {
      testRaw[i] = v[i];
    }

    if (!testInput) {
      filterInitialized = false;
    }

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

        for (
          char* p = lineBuf;
          *p;
          p++
        ) {
          if (*p >= 'a' && *p <= 'z') {
            *p -= 32;
          }
        }

        handleLine(lineBuf);

      } else if (lineLen < sizeof(lineBuf) - 1) {

        lineBuf[lineLen++] = c;

      } else {

        lineMode = false;

        Serial.println(
          "#EVENT line_command_too_long"
        );
      }

      continue;
    }

    if (c == '$') {

      lineMode = true;
      lineLen = 0;

    } else if (c == 'D' || c == 'd' || c == 'W' || c == 'w') {

      // One tap is enough: a repeated tap within 2 s is ignored.
      if (calTapped && millis() - lastCalTapMs < CAL_TAP_GUARD_MS) {
        Serial.println("#EVENT calibration_tap_ignored_wait_2s");
      } else {
        calTapped = true;
        lastCalTapMs = millis();
        captureCalibration(c == 'D' || c == 'd');
      }

    } else if (c == 'L' || c == 'l') {

      ledTestActive = true;
      ledTestMs = millis();

      Serial.println("#EVENT led_test_2s");

    } else if (c == 'X' || c == 'x') {

      stopRelayDiagnostic();

      setPump(false);

      autoEnabled = false;

      setLockout(false);

      setPaused(true, "by_X");

      waterDemand = false;

      pulsesUsed = 0;

      endTestInput("by_X");

      Serial.println(
        "#EVENT stopped_and_lockout_cleared"
      );

    } else if (c == 'A' || c == 'a') {

      if (!PUMP_HARDWARE_VERIFIED) {

        Serial.println(
          "#EVENT auto_denied_check_NO_COM_and_trigger_polarity"
        );

      } else if (safetyLockout) {

        Serial.println(
          "#EVENT auto_denied_lockout_send_X_then_review"
        );

      } else if (!calibrationValid()) {

        Serial.println(
          "#EVENT auto_denied_calibrate_D_then_W"
        );

      } else if (averagePercent() < 0) {

        Serial.println(
          "#EVENT auto_denied_no_usable_sensor"
        );

      } else if (pumpCommandOn) {

        Serial.println(
          "#EVENT auto_denied_pump_busy"
        );

      } else {

        setPaused(false, "by_A");

        autoEnabled = true;

        waterDemand = false;

        pulsesUsed = 0;

        Serial.println("#EVENT auto_enabled");
      }

    } else if (c == 'R' || c == 'r') {

      if (
        PUMP_HARDWARE_VERIFIED ||
        pumpCommandOn ||
        relayTestActive
      ) {

        Serial.println(
          "#EVENT relay_diagnostic_denied_check_state"
        );

      } else {

        relayTestActive = true;

        relayTestStartMs = millis();

        setRelayDiagnosticOutput(true);

        Serial.println(
          "#EVENT relay_diagnostic_started_1p5s_KEEP_PUMP_DISCONNECTED"
        );
      }

    } else if (c == 'T' || c == 't') {

      if (
        !PUMP_HARDWARE_VERIFIED ||
        autoEnabled ||
        safetyLockout ||
        pumpCommandOn
      ) {

        Serial.println(
          "#EVENT manual_pump_test_denied"
        );

      } else {

        manualPulse = true;

        setPump(true);

        Serial.println(
          "#EVENT manual_pump_test_1p5s"
        );
      }

    } else if (c == '?') {

      showHelp();
    }
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  // Set relay to its configured OFF level immediately.
  digitalWrite(
    RELAY_PIN,
    RELAY_ACTIVE_LOW ? HIGH : LOW
  );

  pinMode(RELAY_PIN, OUTPUT);

  pinMode(LED_PIN, OUTPUT);

  digitalWrite(LED_PIN, LOW);

  analogReadResolution(12);

  for (int i = 0; i < 3; i++) {

    pinMode(SENSOR_PINS[i], INPUT);

    analogSetPinAttenuation(
      SENSOR_PINS[i],
      ADC_11db
    );
  }

  prefs.begin("soilcfg", false);

  for (int i = 0; i < 3; i++) {

    dryADC[i] = prefs.getInt(
      DRY_KEYS[i],
      -1
    );

    wetADC[i] = prefs.getInt(
      WET_KEYS[i],
      -1
    );

    // no valid stored calibration for this sensor: use the built-in values
    if (!refsValid(i)) {
      dryADC[i] = DEFAULT_DRY_ADC[i];
      wetADC[i] = DEFAULT_WET_ADC[i];
    }
  }

  // v2.8: a pause (X) no longer survives a restart, so the board always starts working
  // by itself. The safety lockout (3 pulses without reaching the stop level) still does.
  autoPaused = false;
  prefs.putBool("paused", false);

  safetyLockout = prefs.getBool(
    "lockout",
    false
  );

  // LED startup test.
  digitalWrite(LED_PIN, HIGH);

  delay(2000);

  digitalWrite(LED_PIN, LOW);

  settleFromMs = millis();

  sampleSensors();

  updateSoilStatus();

  autoEnabled = autoAllowed();

  Serial.println(
    "#EVENT esp32_s3_irrigation_v2_8_fast_response_boot"
  );

  Serial.println(
    "#EVENT led_startup_test_complete"
  );

  if (!PUMP_HARDWARE_VERIFIED) {

    Serial.println(
      "#EVENT PUMP_DISABLED_UNTIL_HARDWARE_VERIFIED"
    );
  }

  if (!calibrationValid()) {

    Serial.println(
      "#EVENT calibration_needed_send_D_and_W"
    );
  }

  if (autoPaused) {

    Serial.println(
      "#EVENT auto_paused_send_A_to_resume"
    );
  }

  if (safetyLockout) {

    Serial.println(
      "#EVENT lockout_active_inspect_then_send_X"
    );
  }

  if (autoEnabled) {

    Serial.println(
      "#EVENT auto_enabled_at_boot"
    );
  }

  showHelp();

  lastSampleMs = millis();
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  handleSerial();

  unsigned long now = millis();

  // Independent hard time limit for each normal pump pulse.
  if (
    pumpCommandOn &&
    (now - pumpStartMs >= PUMP_PULSE_MS)
  ) {
    setPump(false);
  }

  // Relay diagnostic cutoff.
  if (
    relayTestActive &&
    (now - relayTestStartMs >= PUMP_PULSE_MS)
  ) {
    stopRelayDiagnostic();
  }

  if (
    ledTestActive &&
    (now - ledTestMs >= 2000UL)
  ) {
    ledTestActive = false;
  }

  if (
    testInput &&
    (now - testInputMs >= TEST_INPUT_MAX_MS)
  ) {
    endTestInput("timeout");
  }

  // FAST CONTROL LOOP:
  // Read sensors and evaluate the 98% stop threshold every 100 ms.
  if (
    now - lastSampleMs >= SAMPLE_INTERVAL_MS
  ) {

    lastSampleMs = now;

    sampleSensors();

    updateSoilStatus();

    controlWatering();

  }

  // LED and serial telemetry update once per second.
  if (
    now - lastUiUpdateMs >= UI_UPDATE_INTERVAL_MS
  ) {

    lastUiUpdateMs = now;

    updateLED();

    emitTelemetry();

  } else {

    updateLED();
  }
}

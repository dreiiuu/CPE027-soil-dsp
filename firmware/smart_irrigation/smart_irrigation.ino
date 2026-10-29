/*
  SMART SOIL IRRIGATION - ESP32-S3 N16R8
  Version 1.0 - supervised prototype

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

  Serial 115200 commands (send via Serial Monitor):
    D = save dry-soil references for all three sensors
    W = save wet-soil references for all three sensors
    A = enable auto watering (after calibration and hardware verification)
    X = disable auto and clear software lockout
    L = test external LED for 2 seconds
    T = manual 1.5-second pump pulse (requires hardware verification)
    ? = show commands

  Telemetry is JSONL: one line starting { each second.
  Other diagnostic messages begin with #EVENT.
*/

#include <Arduino.h>
#include <math.h>
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

// -------- VERIFY THESE TWO SETTINGS BEFORE USING THE PUMP --------
// true: relay input turns relay ON when signal is LOW.
// false: relay input turns relay ON when signal is HIGH.
const bool RELAY_ACTIVE_LOW = true;

// Deliberately FALSE until pump COM/NO wiring and active level are verified.
// Change to true ONLY after the pump physically stops when commanded OFF.
const bool PUMP_HARDWARE_VERIFIED = false;

// -------- PROTOTYPE CONTROL SETTINGS --------
// These are percentages relative to dry/wet calibration points,
// NOT laboratory volumetric water content or plant-available water.
const int START_WATERING_PCT = 50;
const int STOP_WATERING_PCT = 70;
const unsigned long SAMPLE_INTERVAL_MS = 1000UL;
const unsigned long PUMP_PULSE_MS = 1500UL;
const unsigned long SOAK_INTERVAL_MS = 20000UL;
const uint8_t MAX_PULSES = 3;
const int MIN_CALIBRATION_SPAN_ADC = 150;
const int ADC_BAD_LOW = 10;
const int ADC_BAD_HIGH = 4085;
const float EMA_ALPHA = 0.25f;

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);
Preferences prefs;
const char* DRY_KEYS[3] = {"dry0", "dry1", "dry2"};
const char* WET_KEYS[3] = {"wet0", "wet1", "wet2"};

int rawADC[3] = {0, 0, 0};
float filteredADC[3] = {0, 0, 0};
bool filterInitialized = false;
int dryADC[3] = {-1, -1, -1};
int wetADC[3] = {-1, -1, -1};

bool sensorFault = false;
bool autoEnabled = false;
bool pumpCommandOn = false;
bool waterDemand = false;
bool safetyLockout = false;
bool ledTestActive = false;
bool ledState = false;
uint8_t pulsesUsed = 0;
unsigned long pumpStartMs = 0;
unsigned long lastPumpStopMs = 0;
unsigned long lastSampleMs = 0;
unsigned long ledTestMs = 0;

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
      Serial.println("#EVENT pump_command_off");
    }
    pumpCommandOn = on;
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
    rawADC[i] = analogAverage(SENSOR_PINS[i], 12);
    if (!filterInitialized) filteredADC[i] = rawADC[i];
    else filteredADC[i] += EMA_ALPHA * (rawADC[i] - filteredADC[i]);
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
  return (sensorPercent(0) + sensorPercent(1) + sensorPercent(2)) / 3;
}

void captureCalibration(bool dry) {
  autoEnabled = false;
  waterDemand = false;
  setPump(false);
  int measured[3];
  for (int i = 0; i < 3; i++) {
    // Capture stable dry/wet reference with repeated samples.
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
  for (int i = 0; i < 3; i++) {
    if (dry) {
      dryADC[i] = measured[i];
      prefs.putInt(DRY_KEYS[i], measured[i]);
    } else {
      wetADC[i] = measured[i];
      prefs.putInt(WET_KEYS[i], measured[i]);
    }
    rawADC[i] = measured[i];
    filteredADC[i] = measured[i];
  }
  filterInitialized = true;
  sensorFault = false;
  Serial.println(dry ? "#EVENT dry_reference_saved" : "#EVENT wet_reference_saved");
  Serial.println(calibrationValid() ? "#EVENT calibration_valid" :
                 "#EVENT calibration_incomplete_or_span_too_small");
}

void controlWatering() {
  // Sensor faults and absent calibration prevent watering.
  if (sensorFault || !calibrationValid()) {
    autoEnabled = false;
    waterDemand = false;
    setPump(false);
    return;
  }

  int pct = averagePercent();
  if (pct < START_WATERING_PCT) waterDemand = true;
  if (pct >= STOP_WATERING_PCT) {
    waterDemand = false;
    pulsesUsed = 0;
    setPump(false);
  }

  if (!autoEnabled || safetyLockout || !waterDemand || pumpCommandOn)
    return;

  if (pulsesUsed >= MAX_PULSES) {
    safetyLockout = true;
    autoEnabled = false;
    setPump(false);
    Serial.println("#EVENT safety_lockout_max_pulses");
    return;
  }

  bool soaked = pulsesUsed == 0 ||
    (millis() - lastPumpStopMs >= SOAK_INTERVAL_MS);
  if (soaked) {
    setPump(true);
    if (pumpCommandOn) pulsesUsed++;
  }
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
  tft.print("SOIL IRRIGATION");
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
              ILI9341_WHITE);
  }
  if (sensorFault) {
    snprintf(buf, sizeof(buf), "FAULT");
    drawValue(3, 72, 135, 238, 17, 2, buf, ILI9341_RED);
  } else if (!calibrationValid()) {
    snprintf(buf, sizeof(buf), "CAL REQUIRED");
    drawValue(3, 72, 135, 238, 17, 2, buf, ILI9341_YELLOW);
  } else {
    snprintf(buf, sizeof(buf), "%d%%", averagePercent());
    drawValue(3, 72, 135, 238, 17, 2, buf,
              waterDemand ? ILI9341_RED : ILI9341_GREEN);
  }
  drawValue(4, 95, 169, 215, 17, 2,
            pumpCommandOn ? "ON" : "OFF",
            pumpCommandOn ? ILI9341_GREEN : ILI9341_RED);
  drawValue(5, 65, 199, 245, 10, 1,
            safetyLockout ? "LOCKOUT" : (autoEnabled ? "AUTO" : "MANUAL"),
            safetyLockout ? ILI9341_RED : ILI9341_CYAN);
  if (!PUMP_HARDWARE_VERIFIED) snprintf(buf, sizeof(buf), "PUMP WIRING NOT VERIFIED");
  else if (sensorFault) snprintf(buf, sizeof(buf), "CHECK SENSOR POWER/AO");
  else if (!calibrationValid()) snprintf(buf, sizeof(buf), "SEND D THEN W");
  else if (safetyLockout) snprintf(buf, sizeof(buf), "MAX PULSES; SEND X");
  else if (waterDemand) snprintf(buf, sizeof(buf), "DRY  PULSES %d/%d", pulsesUsed, MAX_PULSES);
  else snprintf(buf, sizeof(buf), "OK    PULSES %d/%d", pulsesUsed, MAX_PULSES);
  drawValue(6, 65, 219, 245, 10, 1, buf, ILI9341_YELLOW);
}

void emitTelemetry() {
  // JSON Lines (JSONL) for the future DSP / desktop / dashboard software.
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
  Serial.println("}");
}

void showHelp() {
  Serial.println("#EVENT commands D=dry W=wet A=auto X=stop/reset L=LED T=pump ?=help");
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'D' || c == 'd') captureCalibration(true);
    else if (c == 'W' || c == 'w') captureCalibration(false);
    else if (c == 'L' || c == 'l') {
      ledTestActive = true;
      ledTestMs = millis();
      Serial.println("#EVENT led_test_2s");
    } else if (c == 'X' || c == 'x') {
      setPump(false);
      autoEnabled = false;
      safetyLockout = false;
      waterDemand = false;
      pulsesUsed = 0;
      Serial.println("#EVENT stopped_and_lockout_cleared");
    } else if (c == 'A' || c == 'a') {
      if (!PUMP_HARDWARE_VERIFIED) Serial.println("#EVENT auto_denied_check_NO_COM_and_trigger_polarity");
      else if (safetyLockout) Serial.println("#EVENT auto_denied_lockout_send_X_then_review");
      else if (!calibrationValid()) Serial.println("#EVENT auto_denied_calibrate_D_then_W");
      else if (sensorFault) Serial.println("#EVENT auto_denied_sensor_fault");
      else if (pumpCommandOn) Serial.println("#EVENT auto_denied_pump_busy");
      else {
        autoEnabled = true;
        waterDemand = false;  // Only start a new cycle below START threshold.
        pulsesUsed = 0;
        Serial.println("#EVENT auto_enabled");
      }
    } else if (c == 'T' || c == 't') {
      if (!PUMP_HARDWARE_VERIFIED || sensorFault || autoEnabled || safetyLockout)
        Serial.println("#EVENT manual_pump_test_denied");
      else {
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

  initDisplay();
  // LED wiring self-test, regardless of moisture calibration.
  digitalWrite(LED_PIN, HIGH);
  delay(2000);
  digitalWrite(LED_PIN, LOW);

  sampleSensors();
  updateDisplay();
  Serial.println("#EVENT esp32_s3_irrigation_boot_manual_mode");
  Serial.println("#EVENT led_startup_test_complete");
  if (!PUMP_HARDWARE_VERIFIED) Serial.println("#EVENT PUMP_DISABLED_UNTIL_HARDWARE_VERIFIED");
  if (!calibrationValid()) Serial.println("#EVENT calibration_needed_send_D_and_W");
  showHelp();
  lastSampleMs = millis();
}

void loop() {
  handleSerial();
  unsigned long now = millis();

  // Time-based pump cutoff independent of sensor sampling.
  if (pumpCommandOn && (now - pumpStartMs >= PUMP_PULSE_MS)) setPump(false);
  if (ledTestActive && (now - ledTestMs >= 2000UL)) ledTestActive = false;

  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;
    sampleSensors();
    controlWatering();
    updateLED();
    updateDisplay();
    emitTelemetry();
  } else {
    updateLED();
  }
}

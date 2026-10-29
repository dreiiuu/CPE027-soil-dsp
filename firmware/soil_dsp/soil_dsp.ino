/*
  soil_dsp.ino - Two-stage median + EMA smoothing of 3 capacitive soil-moisture sensors
  Board : ESP32-S3 (Arduino-ESP32 core 3.x)   Libraries: Adafruit GFX, Adafruit ST7735 and ST7789
  Files : dsp.h (filters + decision logic), dashboard.h (web page), secrets.h (Wi-Fi, optional)

  Signal path (1 Hz):
    ADC counts -> calibrated index (Eq.2) -> [blanking after relay switch] ->
    cross-sensor median (Eq.3) -> 5-point running median (Eq.4) -> EMA alpha=0.1 (Eq.5)
    moving average M=10 (Eq.6) runs on the same x[n] as the comparison baseline
    y[n] -> hysteresis/lockout/Tmax pump logic (Eq.7) + wet alert -> relay, TFT, web dashboard

  Serial output (115200) is a CSV the analysis script reads directly:
    t_s,s1,s2,s3,pump      raw 12-bit ADC counts, relay state 0/1
  Lines starting with '#' are comments and are ignored by analyze_log.py.

  Serial commands (single character):
    d / w  start 60 s dry (air) / wet (water) calibration     e / x  enable / disable pump control
    t      toggle 3 s on / 3 s off pump test (Test 4)          b      toggle blanking
    r      reset filters and counters                          s      print calibration and parameters
    h      print the CSV header                                p      print pin map + wiring check
    ?      help
*/
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "dsp.h"
#include "dashboard.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

// ---------- TFT DISPLAY (unchanged from the working prototype) ----------
#define TFT_SCK   12
#define TFT_MOSI  11
#define TFT_CS    10
#define TFT_DC    14
#define TFT_RST   21
SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft(&tftSPI, TFT_CS, TFT_DC, TFT_RST);

// ---------- SENSORS (ADC1 only: ADC2 is shared with Wi-Fi) ----------
const int SENSOR_PIN[3] = {2, 1, 3};          // S1 = v1.2 (GPIO2), S2 = v2.0 (GPIO1), S3 = v2.0 (GPIO3)

// ---------- RELAY (active LOW module; swap if yours is reversed) ----------
#define RELAY_PIN 20
const int RELAY_ON  = LOW;
const int RELAY_OFF = HIGH;

// ---------- DSP / timing constants (keep in sync with the report) ----------
const uint32_t SAMPLE_PERIOD_MS = 1000;        // fs = 1 Hz
const float    ALPHA            = 0.1f;        // EMA coefficient
const uint32_t WARMUP_S         = 15;          // no pumping until the filters have settled
const int      CAL_SECONDS      = 60;          // calibration capture length
const int      CAL_SKIP         = 5;           // seconds ignored at the start of a capture
const float    MIN_SPAN         = 300.0f;      // adry-awet smaller than this = probe not really in air/water
const bool     PUMP_ENABLED_AT_BOOT = false;   // safe default: enable from dashboard or serial 'e'
const uint32_t TEST_ON_MS = 3000, TEST_OFF_MS = 3000, TEST_MAX_MS = 600000;
#define ADC_OVERSAMPLE 1                       // 1 = raw single reading (the signal the report filters)

// ---------- wiring check (hardware bring-up aid; does not change the DSP path) ----------
// A capacitive probe on 3.3 V reads roughly 1100-3000 counts. Counts stuck near 0 mean AOUT is not
// reaching the pin or the probe has no power; counts stuck at the top mean AOUT is shorted to 3.3 V.
const int FAULT_LOW_CNT  = 200;
const int FAULT_HIGH_CNT = 4000;
const int FAULT_HOLD_S   = 3;                  // consecutive samples before flagging (a single spike is not a fault)
enum Fault : uint8_t { FAULT_NONE = 0, FAULT_NO_SIGNAL = 1, FAULT_AT_RAIL = 2 };

// ---------- state ----------
Preferences prefs;
WebServer server(80);

float adryCnt[3] = {3000, 3000, 3000};
float awetCnt[3] = {1300, 1300, 1300};
bool  calDry = false, calWet = false;

dsp::Params P;
dsp::Pipeline<5, 10> dspPipe(ALPHA);
dsp::Controller ctl;
dsp::ToggleCounter tog[4] = {dsp::ToggleCounter(35, 0), dsp::ToggleCounter(35, 0),
                             dsp::ToggleCounter(35, 0), dsp::ToggleCounter(35, 0)};

bool pumpEnabled = PUMP_ENABLED_AT_BOOT;
bool blankEnabled = true;
bool testMode = false;
uint32_t testStartMs = 0;
bool relayOn = false;
int64_t lastSwitchMs = -1;
float lastValid[3] = {0, 0, 0};
bool haveValid = false;

uint32_t t0Ms = 0, sampleN = 0, warmupUntil = WARMUP_S;

struct CalState { bool busy = false; bool wet = false; int n = 0; int cnt = 0; double sum[3] = {0, 0, 0}; } cal;

// latest values for the dashboard / TFT
int   lastAdc[3] = {0, 0, 0};
float lastIdx[3] = {0, 0, 0};   // calibrated, before blanking
dsp::PipelineOut lastOut{};
dsp::Status lastStatus = dsp::Status::OK;
String ipText = "";
uint8_t sensorFault[3] = {FAULT_NONE, FAULT_NONE, FAULT_NONE};
uint8_t faultRun[3] = {0, 0, 0};

// history ring buffer for the dashboard (last HIST samples)
const int HIST = 240;
struct Hist { uint32_t t; uint16_t a[3]; float x, y, ma; uint8_t p; } hist[HIST];
int histCount = 0, histHead = 0;

// ============================================================ helpers
static int readAdc(int pin) {
#if ADC_OVERSAMPLE > 1
  long s = 0;
  for (int i = 0; i < ADC_OVERSAMPLE; i++) s += analogRead(pin);
  return (int)(s / ADC_OVERSAMPLE);
#else
  return analogRead(pin);
#endif
}

static const char* faultName(uint8_t f) {
  return f == FAULT_NO_SIGNAL ? "NO SIGNAL" : (f == FAULT_AT_RAIL ? "AT 3.3V" : "OK");
}

static void checkWiring(const int a[3]) {
  for (int i = 0; i < 3; i++) {
    uint8_t f = a[i] < FAULT_LOW_CNT ? FAULT_NO_SIGNAL : (a[i] > FAULT_HIGH_CNT ? FAULT_AT_RAIL : FAULT_NONE);
    if (f == FAULT_NONE) faultRun[i] = 0; else if (faultRun[i] < 255) faultRun[i]++;
    uint8_t now = f;                                   // recovery clears at once, a fault needs FAULT_HOLD_S samples
    if (f != FAULT_NONE && faultRun[i] < FAULT_HOLD_S) now = sensorFault[i];
    if (now != sensorFault[i]) {
      Serial.printf("# wiring: S%d (GPIO%d) %s, %d counts\n", i + 1, SENSOR_PIN[i], faultName(now), a[i]);
      sensorFault[i] = now;
    }
  }
}

static void printPins() {
  Serial.printf("# pins: S1(v1.2)=GPIO%d S2(v2.0)=GPIO%d S3(v2.0)=GPIO%d relay IN=GPIO%d (active %s)\n",
                SENSOR_PIN[0], SENSOR_PIN[1], SENSOR_PIN[2], RELAY_PIN, RELAY_ON == LOW ? "LOW" : "HIGH");
  Serial.printf("# pins: TFT SCK=%d MOSI=%d CS=%d DC=%d RST=%d\n", TFT_SCK, TFT_MOSI, TFT_CS, TFT_DC, TFT_RST);
  for (int i = 0; i < 3 && sampleN > 0; i++)
    Serial.printf("# wiring: S%d now %d counts, %s\n", i + 1, lastAdc[i], faultName(sensorFault[i]));
}

static bool calibrated() {
  if (!calDry || !calWet) return false;
  for (int i = 0; i < 3; i++) if (adryCnt[i] - awetCnt[i] < MIN_SPAN) return false;
  return true;
}

static void setRelay(bool on, uint32_t tms) {
  if (on == relayOn) return;
  relayOn = on;
  digitalWrite(RELAY_PIN, on ? RELAY_ON : RELAY_OFF);
  lastSwitchMs = tms;  // blanking window starts at this tick
}

static void applyThreshold() {
  for (int i = 0; i < 4; i++) tog[i].setThreshold(P.thetaDry);
  ctl.setParams(P);
}

static void resetFilters() {
  dspPipe.reset();
  for (int i = 0; i < 4; i++) tog[i].reset();
  haveValid = false;
  warmupUntil = sampleN + WARMUP_S;
}

static void saveCal() {
  prefs.putBool("cd", calDry); prefs.putBool("cw", calWet);
  for (int i = 0; i < 3; i++) {
    char k[4];
    snprintf(k, sizeof k, "ad%d", i); prefs.putFloat(k, adryCnt[i]);
    snprintf(k, sizeof k, "aw%d", i); prefs.putFloat(k, awetCnt[i]);
  }
}

static void saveParams() {
  prefs.putFloat("thd", P.thetaDry); prefs.putFloat("hy", P.hysteresis); prefs.putFloat("thw", P.thetaWet);
  prefs.putUInt("tmax", P.tMaxMs); prefs.putUInt("lock", P.lockoutMs); prefs.putUInt("blk", P.blankMs);
}

static void loadAll() {
  prefs.begin("soil", false);
  calDry = prefs.getBool("cd", false); calWet = prefs.getBool("cw", false);
  for (int i = 0; i < 3; i++) {
    char k[4];
    snprintf(k, sizeof k, "ad%d", i); adryCnt[i] = prefs.getFloat(k, adryCnt[i]);
    snprintf(k, sizeof k, "aw%d", i); awetCnt[i] = prefs.getFloat(k, awetCnt[i]);
  }
  P.thetaDry = prefs.getFloat("thd", P.thetaDry); P.hysteresis = prefs.getFloat("hy", P.hysteresis);
  P.thetaWet = prefs.getFloat("thw", P.thetaWet);
  P.tMaxMs = prefs.getUInt("tmax", P.tMaxMs); P.lockoutMs = prefs.getUInt("lock", P.lockoutMs);
  P.blankMs = prefs.getUInt("blk", P.blankMs);
}

static void printParams() {
  Serial.printf("# calibrated=%d adry=%.1f,%.1f,%.1f awet=%.1f,%.1f,%.1f\n", (int)calibrated(),
                adryCnt[0], adryCnt[1], adryCnt[2], awetCnt[0], awetCnt[1], awetCnt[2]);
  Serial.printf("# alpha=%.2f N=5 M=10 thetaDry=%.1f h=%.1f thetaWet=%.1f Tmax=%lus lockout=%lus blank=%lus blanking=%d pump_enabled=%d\n",
                ALPHA, P.thetaDry, P.hysteresis, P.thetaWet, (unsigned long)(P.tMaxMs / 1000),
                (unsigned long)(P.lockoutMs / 1000), (unsigned long)(P.blankMs / 1000), (int)blankEnabled, (int)pumpEnabled);
}

static void startCal(bool wet) {
  cal.busy = true; cal.wet = wet; cal.n = 0; cal.cnt = 0;
  for (int i = 0; i < 3; i++) cal.sum[i] = 0;
  setRelay(false, (uint32_t)(sampleN * SAMPLE_PERIOD_MS));
  testMode = false;
  Serial.printf("# calibration %s started (%d s) - keep all three probes %s\n", wet ? "WET" : "DRY", CAL_SECONDS,
                wet ? "in water up to the line" : "in open air");
}

static void calSample(const int a[3]) {
  cal.n++;
  if (cal.n > CAL_SKIP) { for (int i = 0; i < 3; i++) cal.sum[i] += a[i]; cal.cnt++; }
  if (cal.n >= CAL_SECONDS) {
    for (int i = 0; i < 3; i++) {
      float mean = (float)(cal.sum[i] / (cal.cnt > 0 ? cal.cnt : 1));
      if (cal.wet) awetCnt[i] = mean; else adryCnt[i] = mean;
    }
    if (cal.wet) calWet = true; else calDry = true;
    cal.busy = false;
    saveCal();
    resetFilters();
    Serial.printf("# calibration %s done\n", cal.wet ? "WET" : "DRY");
    printParams();
    if (calDry && calWet && !calibrated()) Serial.println("# WARNING: a sensor span is below the minimum - recalibrate it");
  }
}

// ============================================================ TFT
static void tftRow(int y, uint16_t color, const String& s, uint8_t size) {  // no default arg: Arduino auto-prototypes break on them
  tft.fillRect(0, y, 240, size * 8 + 6, ST77XX_BLACK);
  tft.setTextSize(size);
  tft.setTextColor(color, ST77XX_BLACK);
  tft.setCursor(8, y + 2);
  tft.print(s);
}

static void drawTft() {
  uint16_t col = lastStatus == dsp::Status::DRY ? ST77XX_RED : (lastStatus == dsp::Status::WET ? ST77XX_CYAN : ST77XX_GREEN);
  const char* word = lastStatus == dsp::Status::DRY ? "DRY" : (lastStatus == dsp::Status::WET ? "WET" : "OK");
  if (cal.busy) {
    tftRow(48, ST77XX_YELLOW, String("CAL ") + (cal.wet ? "WET" : "DRY"), 4);
    tftRow(96, ST77XX_WHITE, String(cal.n) + "/" + CAL_SECONDS + " s", 2);
  } else {
    tftRow(48, col, word, 4);                       // status as a WORD plus colour
    tftRow(96, ST77XX_WHITE, String("y: ") + String(lastOut.y, 1) + "  x: " + String(lastOut.x, 1), 2);
  }
  for (int i = 0; i < 3; i++) {
    String label = String("S") + (i + 1) + " ";
    if (sensorFault[i]) tftRow(126 + 22 * i, ST77XX_ORANGE, label + faultName(sensorFault[i]) + " " + lastAdc[i], 2);
    else tftRow(126 + 22 * i, ST77XX_WHITE, label + String(lastIdx[i], 1) + "  " + lastAdc[i], 2);
  }
  String mode = testMode ? "TEST" : (!calibrated() ? "LOCKED" : (pumpEnabled ? "AUTO" : "OFF"));
  tftRow(200, relayOn ? ST77XX_YELLOW : ST77XX_WHITE, String("Pump ") + (relayOn ? "ON " : "OFF") + " [" + mode + "]", 2);
  tftRow(222, ST77XX_WHITE, String("Toggles: ") + tog[3].count() + " (raw " + tog[0].count() + ")", 2);
  tftRow(250, calibrated() ? ST77XX_WHITE : ST77XX_ORANGE, calibrated() ? ipText : String("UNCALIBRATED"), 2);
}

// ============================================================ one 1 Hz tick
static void tick(uint32_t tms) {
  int a[3];
  for (int i = 0; i < 3; i++) a[i] = readAdc(SENSOR_PIN[i]);
  for (int i = 0; i < 3; i++) { lastAdc[i] = a[i]; lastIdx[i] = dsp::toIndex(a[i], adryCnt[i], awetCnt[i]); }
  checkWiring(a);

  if (cal.busy) {
    calSample(a);
    Serial.printf("%lu,%d,%d,%d,%d\n", (unsigned long)(tms / 1000), a[0], a[1], a[2], (int)relayOn);
    drawTft();
    return;
  }

  // blanking: readings just after a relay switch are replaced by the last valid value
  float m[3] = {lastIdx[0], lastIdx[1], lastIdx[2]};
  bool blanked = blankEnabled && haveValid && lastSwitchMs >= 0 && (int64_t)tms > lastSwitchMs &&
                 (tms - (uint32_t)lastSwitchMs) < P.blankMs;
  for (int i = 0; i < 3; i++) {
    if (blanked) m[i] = lastValid[i]; else lastValid[i] = m[i];
  }
  haveValid = true;

  lastOut = dspPipe.update(m[0], m[1], m[2]);
  tog[0].update(lastIdx[0]);       // raw sensor 1 (not blanked) - the report's "raw" reference
  tog[1].update(lastOut.x);
  tog[2].update(lastOut.yma);
  tog[3].update(lastOut.y);

  bool want;
  if (testMode) {
    if (tms - testStartMs > TEST_MAX_MS) testMode = false;
    ctl.update(lastOut.y, tms, false);                         // keep the status word fresh
    want = testMode && ((tms - testStartMs) % (TEST_ON_MS + TEST_OFF_MS)) < TEST_ON_MS;  // 3 s on, 3 s off
  } else {
    bool allow = pumpEnabled && calibrated() && sampleN >= warmupUntil;
    want = ctl.update(lastOut.y, tms, allow);
  }
  lastStatus = ctl.status();
  setRelay(want, tms);

  // history for the dashboard
  Hist& h = hist[histHead];
  h.t = tms / 1000; for (int i = 0; i < 3; i++) h.a[i] = (uint16_t)a[i];
  h.x = lastOut.x; h.y = lastOut.y; h.ma = lastOut.yma; h.p = relayOn;
  histHead = (histHead + 1) % HIST; if (histCount < HIST) histCount++;

  Serial.printf("%lu,%d,%d,%d,%d\n", (unsigned long)(tms / 1000), a[0], a[1], a[2], (int)relayOn);
  drawTft();
}

// ============================================================ web
static void sendState() {
  char b[1024];
  snprintf(b, sizeof b,
    "{\"t\":%lu,\"a\":[%d,%d,%d],\"m\":[%.2f,%.2f,%.2f],\"x\":%.2f,\"y\":%.2f,\"ma\":%.2f,"
    "\"status\":\"%s\",\"pump\":%d,\"en\":%d,\"test\":%d,\"blank\":%d,\"calok\":%d,"
    "\"cal\":{\"busy\":%d,\"type\":\"%s\",\"n\":%d},"
    "\"thdry\":%.1f,\"thwet\":%.1f,\"h\":%.1f,\"tmax\":%lu,\"lock\":%lu,\"blankS\":%lu,"
    "\"tog\":[%lu,%lu,%lu,%lu],\"ip\":\"%s\","
    "\"flt\":[%d,%d,%d],\"pins\":{\"s\":[%d,%d,%d],\"relay\":%d,\"relayActiveLow\":%d}}",
    (unsigned long)(sampleN > 0 ? (sampleN - 1) : 0), lastAdc[0], lastAdc[1], lastAdc[2],
    lastIdx[0], lastIdx[1], lastIdx[2], lastOut.x, lastOut.y, lastOut.yma,
    lastStatus == dsp::Status::DRY ? "DRY" : (lastStatus == dsp::Status::WET ? "WET" : "OK"),
    (int)relayOn, (int)pumpEnabled, (int)testMode, (int)blankEnabled, (int)calibrated(),
    (int)cal.busy, cal.wet ? "wet" : "dry", cal.n,
    P.thetaDry, P.thetaWet, P.hysteresis, (unsigned long)(P.tMaxMs / 1000), (unsigned long)(P.lockoutMs / 1000),
    (unsigned long)(P.blankMs / 1000),
    (unsigned long)tog[0].count(), (unsigned long)tog[1].count(), (unsigned long)tog[2].count(), (unsigned long)tog[3].count(),
    ipText.c_str(),
    sensorFault[0], sensorFault[1], sensorFault[2], SENSOR_PIN[0], SENSOR_PIN[1], SENSOR_PIN[2], RELAY_PIN,
    (int)(RELAY_ON == LOW));
  server.send(200, "application/json", b);
}

static void sendHistory() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  const char* keys[6] = {"t", "a", "x", "y", "ma", "p"};
  server.sendContent("{");
  for (int k = 0; k < 6; k++) {
    String s = String("\"") + keys[k] + "\":[";
    for (int i = 0; i < histCount; i++) {
      const Hist& h = hist[(histHead - histCount + i + HIST) % HIST];
      if (i) s += ",";
      switch (k) {
        case 0: s += h.t; break;
        case 1: s += String("[") + h.a[0] + "," + h.a[1] + "," + h.a[2] + "]"; break;
        case 2: s += String(h.x, 2); break;
        case 3: s += String(h.y, 2); break;
        case 4: s += String(h.ma, 2); break;
        default: s += h.p; break;
      }
      if (s.length() > 1000) { server.sendContent(s); s = ""; }
    }
    s += (k < 5) ? "]," : "]";
    server.sendContent(s);
  }
  server.sendContent("}");
  server.sendContent("");
}

static void ok() { server.send(200, "text/plain", "ok"); }
static void bad(const char* why) { server.send(400, "text/plain", why); }

static void setupWeb() {
  server.on("/", []() { server.send_P(200, "text/html", DASHBOARD_HTML); });
  server.on("/api/state", sendState);
  server.on("/api/history", sendHistory);
  server.on("/api/pump", []() { pumpEnabled = server.arg("en") == "1"; ok(); });
  server.on("/api/blank", []() { blankEnabled = server.arg("en") == "1"; ok(); });
  server.on("/api/test", []() {
    bool on = server.arg("on") == "1";
    if (on && !testMode) testStartMs = (uint32_t)(sampleN * SAMPLE_PERIOD_MS);
    testMode = on && !cal.busy;
    ok();
  });
  server.on("/api/cal", []() {
    String ty = server.arg("type");
    if (ty != "dry" && ty != "wet") { bad("type=dry|wet"); return; }
    startCal(ty == "wet");
    ok();
  });
  server.on("/api/reset_toggles", []() { for (int i = 0; i < 4; i++) tog[i].reset(); ok(); });
  server.on("/api/params", []() {
    float d = server.arg("thdry").toFloat(), h = server.arg("h").toFloat(), w = server.arg("thwet").toFloat();
    float tmax = server.arg("tmax").toFloat(), lk = server.arg("lock").toFloat(), bl = server.arg("blank").toFloat();
    if (d < 1 || d > 99 || h < 1 || h > 40 || w <= d + h || w > 100) { bad("need 1<=thdry<99, 1<=h<=40, thwet>thdry+h"); return; }
    if (tmax < 1 || tmax > 300 || lk < 0 || lk > 3600 || bl < 0 || bl > 30) { bad("tmax 1-300 s, lock 0-3600 s, blank 0-30 s"); return; }
    P.thetaDry = d; P.hysteresis = h; P.thetaWet = w;
    P.tMaxMs = (uint32_t)(tmax * 1000); P.lockoutMs = (uint32_t)(lk * 1000); P.blankMs = (uint32_t)(bl * 1000);
    applyThreshold(); saveParams(); printParams(); ok();
  });
  server.begin();
}

static void setupWifi() {
  ipText = "no network";
  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(250);
    if (WiFi.status() == WL_CONNECTED) { ipText = WiFi.localIP().toString(); return; }
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP("SoilDSP", "soildsp123");
  ipText = WiFi.softAPIP().toString() + " (AP)";
}

// ============================================================ serial commands
static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case 'd': startCal(false); break;
      case 'w': startCal(true); break;
      case 'e': pumpEnabled = true;  Serial.println("# pump control ENABLED"); break;
      case 'x': pumpEnabled = false; Serial.println("# pump control DISABLED"); break;
      case 't':
        testMode = !testMode && !cal.busy;
        if (testMode) testStartMs = (uint32_t)(sampleN * SAMPLE_PERIOD_MS);
        Serial.printf("# pump test cycle %s\n", testMode ? "ON" : "OFF");
        break;
      case 'b': blankEnabled = !blankEnabled; Serial.printf("# blanking %s\n", blankEnabled ? "ON" : "OFF"); break;
      case 'r': resetFilters(); Serial.println("# filters reset"); break;
      case 's': printParams(); break;
      case 'h': Serial.println("t_s,s1,s2,s3,pump"); break;
      case 'p': printPins(); break;
      case '?': Serial.println("# d/w calibrate dry/wet | e/x pump enable/disable | t pump test | b blanking | r reset | s status | h header | p pins"); break;
      default: break;
    }
  }
}

// ============================================================ arduino entry points
void setup() {
  Serial.begin(115200);
  delay(500);

  // pump OFF before anything else
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  tftSPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setRotation(0);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 10);
  tft.println("Soil Monitor");
  tft.drawFastHLine(8, 34, 224, ST77XX_WHITE);

  analogReadResolution(12);
  for (int i = 0; i < 3; i++) analogSetPinAttenuation(SENSOR_PIN[i], ADC_11db);

  loadAll();
  applyThreshold();
  tftRow(60, ST77XX_WHITE, "Connecting Wi-Fi...", 2);
  setupWifi();
  setupWeb();
  tft.fillRect(0, 44, 240, 270, ST77XX_BLACK);

  Serial.println("# ESP32-S3 soil DSP started");
  printParams();
  printPins();
  Serial.println("t_s,s1,s2,s3,pump");
  t0Ms = millis();
}

void loop() {
  server.handleClient();
  handleSerial();

  uint32_t now = millis() - t0Ms;
  uint32_t due = sampleN * SAMPLE_PERIOD_MS;
  if (now >= due) {
    if (now - due > 3 * SAMPLE_PERIOD_MS) {            // fell far behind: skip instead of bursting
      sampleN = now / SAMPLE_PERIOD_MS;
      due = sampleN * SAMPLE_PERIOD_MS;
    }
    tick(due);                                         // nominal time keeps t_s an exact 1 Hz grid
    sampleN++;
  }
}

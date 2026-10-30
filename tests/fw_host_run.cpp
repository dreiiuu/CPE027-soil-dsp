// Runs the REAL smart_irrigation.ino on the PC with stubbed hardware and prints exactly what it
// would send over USB serial. tests/test_irrigation_ui.py feeds that output into the PC app, so the
// firmware -> software link is tested end to end without a board.
//
// Two builds (see Makefile, app-test):
//   default         the .ino as shipped (PUMP_HARDWARE_VERIFIED=false): the relay must never turn on
//   VERIFIED_BUILD  a copy with PUMP_HARDWARE_VERIFIED=true: checks the automatic watering logic
#ifndef FW_FILE
#define FW_FILE "../firmware/smart_irrigation/smart_irrigation.ino"
#endif
#include FW_FILE
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

// invented probe counts (dry soil / wet soil) for the scenario; v1.2 probes read lower when wetter
static const int DRYC[3] = {2950, 3380, 3400};
static const int WETC[3] = {1480, 1700, 1720};
static const char* CAL_LINE = "$CAL 2950 3380 3400 1480 1700 1720\n";

[[maybe_unused]] static void setCounts(int a, int b, int c) { g_adc[1] = a; g_adc[2] = b; g_adc[3] = c; }  // board labels 1,2,3
static int countAt(int i, float p) { return (int)lroundf(DRYC[i] + p / 100.0f * (WETC[i] - DRYC[i])); }
static void setPct(float p) { for (int i = 0; i < 3; i++) g_adc[SENSOR_PINS[i]] = countAt(i, p); }
static bool relayOn() { return g_pin[RELAY_PIN] == (RELAY_ACTIVE_LOW ? LOW : HIGH); }

// pump bookkeeping, checked every 10 ms of simulated time
static unsigned long onSince = 0, lastOff = 0, longestOn = 0, shortestGap = 999999;
static int switchOns = 0;
static bool wasOn = false;

static void run(int seconds) {
  for (int k = 0; k < seconds * 100; k++) {
    g_ms += 10;
    loop();
    bool on = relayOn();
    CHECK(on == pumpCommandOn);                              // pin and reported command agree
    if (on && !wasOn) {
      switchOns++;
      if (lastOff) shortestGap = std::min(shortestGap, g_ms - lastOff);
      onSince = g_ms;
    }
    if (!on && wasOn) { lastOff = g_ms; longestOn = std::max(longestOn, g_ms - onSince); }
    wasOn = on;
    if (!PUMP_HARDWARE_VERIFIED && on) { CHECK(!on); return; }   // shipped build: never ON
  }
}
static void cmd(const char* c) { g_serial_in += c; }
static void reboot() {           // power cycle: RAM state lost, NVS kept, uptime restarts
  g_ms = 0; filterInitialized = statusKnown = false; autoEnabled = pumpCommandOn = manualPulse = waterDemand = false;
  testInput = ledTestActive = false; pulsesUsed = 0; lastPumpStopMs = 0; lineMode = false;
  memset(fieldCache, 0, sizeof fieldCache);
  wasOn = false;
  setup();
}

#ifndef VERIFIED_BUILD
// ---------------------------------------------------------------- shipped firmware
int main() {
  g_serial_echo = true;
  setCounts(2580, 3166, 3197);                    // the raw readings reported in the handoff
  setup();
  CHECK(g_pin[RELAY_PIN] == HIGH);                // relay de-energised at boot
  run(5);                                         // uncalibrated: raw only, percentages null
  CHECK(!autoEnabled);
  setCounts(DRYC[0], DRYC[1], DRYC[2]); cmd("D"); run(2);
  setCounts(WETC[0], WETC[1], WETC[2]); cmd("W"); run(2);
  CHECK(calibrationValid() && !autoPaused && !autoEnabled);   // hardware not verified: no AUTO
  setPct(62); run(8);
  setPct(40); run(6);                             // below 50 %: water needed, LED on
  CHECK(waterDemand && g_pin[LED_PIN] == HIGH);
  CHECK(statusKnown && soilDry);                  // DRY below the start threshold
  cmd("A"); run(2); CHECK(!autoEnabled);          // denied: hardware not verified
  cmd("T"); run(2); CHECK(!pumpCommandOn);
  g_adc[3] = 0; run(3);                           // S3 AO disconnected -> fault, paused
  CHECK(sensorFault && autoPaused);
  setPct(75); run(4);
  CHECK(!sensorFault && !waterDemand && statusKnown && !soilDry);   // WET from the stop threshold
  cmd("$RAW 2800 3200 3220\n"); run(3);           // bench test input
  CHECK(testInput && rawADC[0] == 2800);
  cmd("$RAW OFF\n"); run(2); CHECK(!testInput);
  cmd("$CAL 10 1 2 3 4 5\n"); run(1);             // rejected: rail values / spans
  CHECK(dryADC[0] == DRYC[0]);
  cmd(CAL_LINE); run(2);
  cmd("L"); run(3);
  cmd("X"); cmd("?"); run(2);
  CHECK(autoPaused);
  reboot(); run(3);
  CHECK(calibrationValid() && autoPaused);        // both survived the power cycle
  CHECK(switchOns == 0);
  std::fflush(stdout);
  if (failures) std::fprintf(stderr, "%d firmware host-run check(s) FAILED\n", failures);
  return failures ? 1 : 0;
}
#else
// ---------------------------------------------------------------- PUMP_HARDWARE_VERIFIED=true copy
int main() {
  g_serial_echo = true;
  setPct(60);
  setup(); run(3);
  CHECK(!autoEnabled);                            // not calibrated yet
  setPct(40);
  cmd(CAL_LINE); run(4);                          // typed-in references arm AUTO (v2) ...
  CHECK(calibrationValid() && autoEnabled && !autoPaused && waterDemand);
  CHECK(switchOns == 0);                          // ... but nothing pumps during the 5 s settling
  run(2); CHECK(switchOns == 1);

  // dry soil that never gets wetter (empty reservoir): 3 pulses, soak each, then lockout
  setPct(40);
  run(80);
  CHECK(switchOns == 3);
  CHECK(longestOn <= PUMP_PULSE_MS + 10 && longestOn >= PUMP_PULSE_MS - 10);
  CHECK(shortestGap >= SOAK_INTERVAL_MS);
  CHECK(safetyLockout && !autoEnabled && !pumpCommandOn);

  // lockout survives a reboot, and A cannot clear it
  reboot(); run(10);
  CHECK(safetyLockout && !autoEnabled && switchOns == 3);
  cmd("A"); run(1); CHECK(!autoEnabled);
  cmd("X"); run(1);                               // clears lockout, pauses
  CHECK(!safetyLockout && autoPaused && !autoEnabled);
  reboot(); run(3);
  CHECK(autoPaused && !autoEnabled);              // pause survives a reboot too

  // manual pulse runs its full 1.5 s even at 75 % (v1 cut it short)
  setPct(75); run(3);
  longestOn = 0; cmd("T"); run(3);
  CHECK(longestOn >= PUMP_PULSE_MS - 10 && switchOns == 4);

  // A resumes; wet soil: no watering
  cmd("A"); run(5);
  CHECK(autoEnabled && !waterDemand && switchOns == 4);

  // automatic start after a reboot, first pulse not before the 5 s settling time
  setPct(40);
  reboot();
  CHECK(autoEnabled);
  unsigned long boot = settleFromMs;
  run(4); CHECK(switchOns == 4);
  run(3); CHECK(switchOns == 5 && onSince - boot >= BOOT_SETTLE_MS);
  setPct(75); run(25);                            // watering worked: demand cleared, no lockout
  CHECK(!waterDemand && !safetyLockout && pulsesUsed == 0 && switchOns == 5);

  // bench test with $RAW: pretend 45 % without touching the soil
  char line[64];
  snprintf(line, sizeof line, "$RAW %d %d %d\n", countAt(0, 45), countAt(1, 45), countAt(2, 45));
  cmd(line); run(8);
  CHECK(testInput && waterDemand && switchOns == 6);
  cmd("$RAW OFF\n"); run(6);
  CHECK(!testInput && !waterDemand);              // back on the real (wet) probes

  // sensor fault: pump off, AUTO paused until A
  g_adc[2] = 4095; run(3);
  CHECK(sensorFault && autoPaused && !autoEnabled && !pumpCommandOn);
  setPct(75); run(3);
  CHECK(!sensorFault && !autoEnabled);
  cmd("A"); run(2); CHECK(autoEnabled);
  std::fflush(stdout);
  if (failures) std::fprintf(stderr, "%d verified-build check(s) FAILED\n", failures);
  return failures ? 1 : 0;
}
#endif

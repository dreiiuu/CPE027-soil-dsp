// Runs the REAL firmware/smart_irrigation/smart_irrigation.ino on the PC with stubbed hardware and
// prints exactly what it would send over USB serial. tests/test_irrigation_ui.py feeds that output
// into the PC app, so the firmware -> software link is tested end to end without a board.
//   g++ -std=gnu++17 -include Arduino.h -I tests/arduino_stubs tests/fw_host_run.cpp -o build/fw_host_run
#include "../firmware/smart_irrigation/smart_irrigation.ino"
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

// invented probe counts (dry soil / wet soil) for the scenario; v1.2 probes read lower when wetter
static const int DRYC[3] = {2950, 3380, 3400};
static const int WETC[3] = {1480, 1700, 1720};

static void setCounts(int a, int b, int c) { g_adc[1] = a; g_adc[2] = b; g_adc[3] = c; }  // board labels 1,2,3
static void setPct(float p) {
  for (int i = 0; i < 3; i++) g_adc[SENSOR_PINS[i]] = (int)lroundf(DRYC[i] + p / 100.0f * (WETC[i] - DRYC[i]));
}
static void run(int seconds) {
  for (int k = 0; k < seconds * 100; k++) {      // loop() every 10 ms of simulated time
    g_ms += 10;
    loop();
    // PUMP_HARDWARE_VERIFIED=false: the relay (active LOW on pin 6) must never be driven ON
    if (g_pin[6] != HIGH) { CHECK(g_pin[6] == HIGH); return; }
  }
}
static void cmd(const char* c) { g_serial_in += c; }

int main() {
  g_serial_echo = true;
  setCounts(2580, 3166, 3197);                    // the raw readings reported in the handoff
  setup();
  CHECK(g_pin[6] == HIGH);                        // relay de-energised at boot
  run(5);                                         // uncalibrated: raw only, percentages null
  setCounts(DRYC[0], DRYC[1], DRYC[2]); cmd("D"); run(2);
  setCounts(WETC[0], WETC[1], WETC[2]); cmd("W"); run(2);
  CHECK(calibrationValid());
  setPct(62); run(8);                             // between thresholds
  setPct(40); run(6);                             // below 50 %: water needed, LED on
  CHECK(waterDemand && g_pin[LED_PIN] == HIGH);
  cmd("A"); run(2);                               // denied: hardware not verified
  CHECK(!autoEnabled);
  cmd("T"); run(2);                               // denied as well
  CHECK(!pumpCommandOn);
  g_adc[3] = 0; run(3);                           // S3 AO disconnected -> fault, percentages null
  CHECK(sensorFault);
  setPct(75); run(4);                             // recovered, above 70 %: demand cleared
  CHECK(!sensorFault && !waterDemand);
  cmd("L"); run(3);
  cmd("X"); cmd("?"); run(2);
  // power cycle: uptime restarts, calibration comes back from NVS
  g_ms = 0; filterInitialized = false;
  setup(); run(3);
  CHECK(calibrationValid());
  std::fflush(stdout);
  if (failures) std::fprintf(stderr, "%d firmware host-run check(s) FAILED\n", failures);
  return failures ? 1 : 0;
}

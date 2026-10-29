// Runs the REAL soil_dsp.ino on the PC with stubbed hardware and scripted sensor values.
//   g++ -std=gnu++17 -O1 -include Arduino.h -I tests/arduino_stubs -I firmware/soil_dsp tests/host_sim.cpp -o build/host_sim
// Scenario: calibrate dry (air), calibrate wet (water), then moisture sits OK -> falls DRY (pump must start,
// hysteresis stop, lockout hold) -> rises WET (alert) ; also checks the CSV format and relay polarity.
#include "../firmware/soil_dsp/soil_dsp.ino"
#include <cassert>
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

// sensor i reads this index; counts follow the calibration below (v1.2 and v2.0 differ)
static const int DRY[3] = {2950, 2780, 2810};
static const int WET[3] = {1250, 1180, 1210};
static float g_idx = 50.0f;
static int g_spikeSensor = -1, g_spikeCnt = 0;

static void setSensors() {
  for (int i = 0; i < 3; i++) {
    int pin = SENSOR_PIN[i];
    g_adc[pin] = (int)std::lround(DRY[i] - g_idx / 100.0f * (DRY[i] - WET[i]));
  }
  if (g_spikeSensor >= 0 && g_spikeCnt > 0) { g_adc[SENSOR_PIN[g_spikeSensor]] = 500; g_spikeCnt--; }
}

static void runSeconds(int s) {
  for (int k = 0; k < s * 10; k++) {          // loop() called every 100 ms of simulated time
    g_ms += 100;
    if (k % 10 == 0) setSensors();
    loop();
  }
}

int main() {
  setup();
  // --- calibration through the serial command path
  g_idx = 0;   g_serial_in = "d"; runSeconds(61);
  CHECK(!cal.busy && calDry && !calWet);
  g_idx = 100; g_serial_in = "w"; runSeconds(61);
  CHECK(!cal.busy && calWet && calibrated());
  CHECK(std::fabs(adryCnt[0] - 2950) < 2 && std::fabs(awetCnt[2] - 1210) < 2);

  // --- OK region, pump control disabled by default
  g_idx = 55; runSeconds(40);
  CHECK(lastStatus == dsp::Status::OK && !relayOn);

  // --- drying with pump control still disabled: status DRY but the relay must stay off
  g_idx = 20; runSeconds(40);
  CHECK(lastStatus == dsp::Status::DRY && !relayOn);

  // --- enable: pump starts (active LOW relay)
  g_serial_in = "e"; runSeconds(3);
  CHECK(relayOn && g_relay_pin_state == LOW);
  // a one-sample spike on one sensor must not stop/start anything
  g_spikeSensor = 1; g_spikeCnt = 1; runSeconds(2);
  CHECK(relayOn);
  // Tmax (15 s) stops the pump even though the soil still reads dry (stuck-sensor protection)
  int guard = 0, ranFor = 0;
  while (relayOn && guard++ < 40) { runSeconds(1); ranFor++; }
  CHECK(!relayOn && g_relay_pin_state == HIGH);
  CHECK(ranFor + 5 <= 20);                      // stopped by Tmax (15 s), well before 40 s
  // lockout (120 s): still dry but must not restart
  runSeconds(115);
  CHECK(!relayOn);
  runSeconds(10);                               // lockout elapsed -> restarts
  CHECK(relayOn);
  // moisture recovers above theta_dry + h: hysteresis stop, before Tmax
  g_idx = 70;
  int up = 0;
  while (relayOn && up < 30) { runSeconds(1); up++; }
  CHECK(!relayOn && up < 15 && lastOut.y >= P.thetaDry + P.hysteresis - 0.2f);

  // --- wet alert (pump never acts on the wet side)
  g_idx = 92; runSeconds(40);
  CHECK(lastStatus == dsp::Status::WET && !relayOn);

  // --- test mode: 3 s on / 3 s off, then blanking holds readings after each switch
  g_idx = 55; runSeconds(30);
  g_serial_in = "t"; runSeconds(1);
  int on = 0, off = 0, sw = 0; bool prev = relayOn;
  for (int k = 0; k < 120; k++) { runSeconds(1); (relayOn ? on : off)++; if (relayOn != prev) sw++; prev = relayOn; }
  CHECK(sw >= 38 && sw <= 42);                // 120 s / 3 s = 40 switches
  CHECK(std::abs(on - off) <= 4);
  g_serial_in = "t"; runSeconds(2);
  CHECK(!relayOn);

  // --- wiring check: a 1-2 s dropout is not a fault, a sustained one is, and it clears on recovery
  CHECK(sensorFault[0] == FAULT_NONE && sensorFault[1] == FAULT_NONE && sensorFault[2] == FAULT_NONE);
  g_spikeSensor = 2; g_spikeCnt = 2; runSeconds(3);
  CHECK(sensorFault[2] == FAULT_NONE);
  for (int k = 0; k < 5; k++) { g_ms += 1000; g_adc[SENSOR_PIN[1]] = 0; loop(); }
  CHECK(sensorFault[1] == FAULT_NO_SIGNAL);
  for (int k = 0; k < 5; k++) { g_ms += 1000; g_adc[SENSOR_PIN[0]] = 4095; g_adc[SENSOR_PIN[1]] = 0; loop(); }
  CHECK(sensorFault[0] == FAULT_AT_RAIL);
  runSeconds(2);
  CHECK(sensorFault[0] == FAULT_NONE && sensorFault[1] == FAULT_NONE);

  std::printf(failures ? "%d FAILED\n" : "host simulation of soil_dsp.ino passed\n", failures);
  return failures ? 1 : 0;
}

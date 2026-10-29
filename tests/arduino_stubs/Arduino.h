// Minimal host stubs so the .ino can be syntax/type-checked with g++ (no hardware, no Arduino core).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <functional>
#include <string>
#include <unistd.h>   // the real ESP32 core pulls this in (it declares pipe(), read(), write()...), so name clashes show up here too
#include <math.h>
#define LOW 0
#define HIGH 1
#define OUTPUT 1
#define INPUT 0
#define ADC_11db 3
#define PROGMEM
inline int g_relay_pin_state = -1;
inline int g_pin[64];   // last level written to each pin
inline void digitalWrite(int pin, int v) { if (pin == 20) g_relay_pin_state = v; if (pin >= 0 && pin < 64) g_pin[pin] = v; }
inline void pinMode(int, int) {}
inline int g_adc[16] = {0};
inline int analogRead(int pin) { return g_adc[pin]; }
inline void analogReadResolution(int) {}
inline void analogSetPinAttenuation(int, int) {}
inline unsigned long g_ms = 0;
inline unsigned long millis() { return g_ms; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned) {}
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

class String {
 public:
  std::string s;
  String() {}
  String(const char* c) : s(c) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(float v, int d = 2) { char b[32]; snprintf(b, sizeof b, "%.*f", d, v); s = b; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o) { s += o; return *this; }
  template <class T> String& operator+=(T v) { s += String(v).s; return *this; }
  bool operator==(const char* o) const { return s == o; }
  bool operator!=(const char* o) const { return s != o; }
  const char* c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  float toFloat() const { return (float)atof(s.c_str()); }
};
inline String operator+(const String& a, const String& b) { String r(a); r.s += b.s; return r; }
inline String operator+(const String& a, const char* b) { String r(a); r.s += b; return r; }
inline String operator+(const char* a, const String& b) { String r(a); r.s += b.s; return r; }
template <class T> String operator+(const String& a, T v) { String r(a); r.s += String(v).s; return r; }

inline std::string g_serial_in;
inline int g_relay = -1;
inline bool g_serial_echo = false;
struct HardwareSerial {
  void begin(int) {}
  int available() { return (int)g_serial_in.size(); }
  int read() { if (g_serial_in.empty()) return -1; int c = (unsigned char)g_serial_in[0]; g_serial_in.erase(0, 1); return c; }
  // print/println are silent unless g_serial_echo is set (soil_dsp only uses printf for its CSV)
  void print(const char* s) { if (g_serial_echo) fputs(s, stdout); }
  void print(char c) { if (g_serial_echo) fputc(c, stdout); }
  void print(int v) { if (g_serial_echo) printf("%d", v); }
  void print(unsigned v) { if (g_serial_echo) printf("%u", v); }
  void print(long v) { if (g_serial_echo) printf("%ld", v); }
  void print(unsigned long v) { if (g_serial_echo) printf("%lu", v); }
  void println(const char* s = "") { if (g_serial_echo) { fputs(s, stdout); fputc('\n', stdout); } }
  int printf(const char* f, ...) __attribute__((format(printf, 2, 3))) { va_list a; va_start(a, f); int n = vprintf(f, a); va_end(a); return n; }
};
static HardwareSerial Serial;

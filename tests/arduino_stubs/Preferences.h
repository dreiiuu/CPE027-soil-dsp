#pragma once
#include "Arduino.h"
#include <map>
#include <string>
inline std::map<std::string, int> g_prefs_int;   // NVS stand-in (survives a simulated reboot)
struct Preferences {
  bool begin(const char*, bool) { return true; }
  bool getBool(const char*, bool d) { return d; }
  float getFloat(const char*, float d) { return d; }
  uint32_t getUInt(const char*, uint32_t d) { return d; }
  int getInt(const char* k, int d) { auto it = g_prefs_int.find(k); return it == g_prefs_int.end() ? d : it->second; }
  void putInt(const char* k, int v) { g_prefs_int[k] = v; }
  void putBool(const char*, bool) {} void putFloat(const char*, float) {} void putUInt(const char*, uint32_t) {}
};

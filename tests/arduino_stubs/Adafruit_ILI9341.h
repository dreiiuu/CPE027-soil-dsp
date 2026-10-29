#pragma once
#include "Arduino.h"
#define ILI9341_BLACK 0x0000
#define ILI9341_WHITE 0xFFFF
#define ILI9341_RED 0xF800
#define ILI9341_GREEN 0x07E0
#define ILI9341_CYAN 0x07FF
#define ILI9341_DARKCYAN 0x03EF
#define ILI9341_YELLOW 0xFFE0
struct Adafruit_ILI9341 {
  Adafruit_ILI9341(int, int, int, int, int) {}
  void begin() {}
  void setRotation(int) {}
  void fillScreen(uint16_t) {}
  void setTextColor(uint16_t) {}
  void setTextSize(uint8_t) {}
  void setCursor(int, int) {}
  void print(const char*) {}
  void fillRect(int, int, int, int, uint16_t) {}
  void drawFastHLine(int, int, int, uint16_t) {}
};

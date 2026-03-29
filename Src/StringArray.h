#pragma once
#include <Arduino.h>

// Minimal stub to satisfy ESPAsyncWebServer dependency when building
class StringArray {
public:
  StringArray() {}
  size_t length() const { return 0; }
  const String& operator[](size_t) const { static String s; return s; }
  void add(const String& s) { (void)s; }
};

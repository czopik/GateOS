#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Persist panic info (PC, backtrace, task) + recent breadcrumbs across a crash
// reset, so remote diagnostics can show why the previous run died.
void crashLogBegin();                 // first thing in setup()
void crashLogCrumb(const char* text); // any task, short text
void crashLogFill(JsonObject out);    // previous run's crash + crumbs

#include <Arduino.h>
extern void app_setup();
extern void app_loop();
void setup() { app_setup(); }
void loop() { app_loop(); }

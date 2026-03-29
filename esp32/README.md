# ESP32 Gate Controller

This directory contains the high-level GateOS firmware for the ESP32.

## Contents

- [`esp32/Src/`](esp32/Src) — gate logic, web/API, homing, remotes, LED, config, MQTT, OTA
- [`esp32/data/`](esp32/data) — web UI assets and sample config
- [`esp32/lib/`](esp32/lib) — local libraries used by the ESP32 build
- [`esp32/scripts/`](esp32/scripts) — ESP32-oriented diagnostics and regression tooling
- [`esp32/platformio.ini`](esp32/platformio.ini) — PlatformIO configuration for ESP32 builds

## Build

Run from [`esp32/`](esp32):

```bash
python -m platformio run -e esp32
```

## Upload

```bash
python -m platformio run -e esp32 -t upload --upload-port COM3
python -m platformio run -e esp32 -t uploadfs --upload-port COM3
```

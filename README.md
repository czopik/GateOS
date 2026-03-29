# GateOS

GateOS is a split firmware project for automated gate control built around two cooperating controllers:

- [`esp32/`](esp32) — high-level application logic, web UI, API, remotes, LED, homing, diagnostics and OTA
- [`stm32/`](stm32) — low-level hoverboard / BLDC motor control and telemetry firmware

## Repository layout

```text
GateOS/
├─ README.md
├─ LICENSE
├─ .gitignore
├─ docs/
├─ esp32/
└─ stm32/
```

## Architecture

### [`esp32/`](esp32)

The ESP32 side is the supervisory controller. It is responsible for:

- semantic gate commands: OPEN / CLOSE / STOP
- startup homing and reference recovery
- position tracking and runtime state
- web UI, HTTP API and WebSocket status
- LED signalling, remotes, OTA and optional MQTT
- UART supervision of the STM32 controller

See:

- [`esp32/README.md`](esp32/README.md)
- [`esp32/Src/app_main.cpp`](esp32/Src/app_main.cpp)
- [`esp32/Src/gate_controller.cpp`](esp32/Src/gate_controller.cpp)

### [`stm32/`](stm32)

The STM32 side is the low-level motor controller. It is responsible for:

- hoverboard / BLDC drive control
- UART command reception from ESP32
- telemetry generation (`TEL,...`)
- RPM / distance / fault / current reporting

See:

- [`stm32/README.md`](stm32/README.md)
- [`stm32/Src/main.c`](stm32/Src/main.c)

## Documentation

Project documentation is stored in [`docs/`](docs):

- [`docs/ARCHITEKTURA.md`](docs/ARCHITEKTURA.md)
- [`docs/API.md`](docs/API.md)
- [`docs/TESTY.md`](docs/TESTY.md)
- [`docs/HOMING_RECOVERY.md`](docs/HOMING_RECOVERY.md)
- [`docs/LED_LOGIC.md`](docs/LED_LOGIC.md)
- [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md)
- [`docs/CHANGELOG.md`](docs/CHANGELOG.md)

## ESP32 build

Run from the repository root:

```bash
python -m platformio run -e esp32 -d esp32
```

## ESP32 upload

```bash
python -m platformio run -e esp32 -d esp32 -t upload --upload-port COM3
python -m platformio run -e esp32 -d esp32 -t uploadfs --upload-port COM3
```

## STM32 build

The STM32 source tree is present in [`stm32/Src/`](stm32/Src), but this repository does not currently contain a fully wired STM32 build project/toolchain definition. The real build method depends on the STM32 environment used for this firmware.

## Current status

- ESP32 build verified after repository split
- STM32 sources separated cleanly into their own directory
- project documentation moved to [`docs/`](docs)
- repository layout cleaned for public GitHub use

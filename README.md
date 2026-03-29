# GateOS — ESP32 + STM32 Gate Controller

## Repository layout

- [`esp32/`](esp32) — high-level GateOS firmware for the ESP32
- [`stm32/`](stm32) — low-level hoverboard / BLDC motor controller firmware for STM32
- root docs — architecture, API, tests, homing/recovery, LED logic, known issues

GateOS is a two-part gate automation project built around an [`ESP32`](platformio.ini) supervisory controller and an [`STM32`](Src/main.c) hoverboard motor controller firmware. The system provides web control, distance-based motion, startup homing, telemetry recovery, LED signalling, remotes, diagnostics and test tooling.

## Project goals

- stable semantic gate control: OPEN / CLOSE / STOP
- safe startup after reboot with homing to CLOSE reference
- UART supervision over an STM32 hoverboard power stage
- web UI + HTTP API + WebSocket status updates
- optional MQTT / Home Assistant style integration
- hardware safety: photocell, limit switches, watchdogs, current limits

## High-level architecture

### ESP32 layer

The ESP32 application entrypoint is implemented in [`Src/app_main.cpp`](Src/app_main.cpp). It is responsible for:

- config loading and persistence
- Wi-Fi, OTA, MQTT and web server
- gate state machine orchestration
- startup homing / recovery after reboot
- telemetry normalization from the STM32 controller
- remote learning and LED status indication

Core ESP32 modules:

- [`Src/app_main.cpp`](Src/app_main.cpp) — runtime bootstrap, startup homing, global diagnostics, OTA lifecycle
- [`Src/gate_controller.cpp`](Src/gate_controller.cpp) / [`Src/gate_controller.h`](Src/gate_controller.h) — semantic gate motion control
- [`Src/motor_controller.cpp`](Src/motor_controller.cpp) / [`Src/motor_controller.h`](Src/motor_controller.h) — PWM / hover UART abstraction and motion profiles
- [`Src/position_tracker.cpp`](Src/position_tracker.cpp) / [`Src/position_tracker.h`](Src/position_tracker.h) — position persistence, resync, hover/hall distance tracking
- [`Src/calibration_manager.cpp`](Src/calibration_manager.cpp) / [`Src/calibration_manager.h`](Src/calibration_manager.h) — manual calibration workflow including direction detection
- [`Src/web_server.cpp`](Src/web_server.cpp) / [`Src/web_server.h`](Src/web_server.h) — HTTP + WebSocket API and static UI
- [`Src/config_manager.cpp`](Src/config_manager.cpp) / [`Src/config_manager.h`](Src/config_manager.h) — JSON config model and save/load
- [`Src/hover_uart_driver.cpp`](Src/hover_uart_driver.cpp) / [`Src/hover_uart_driver.h`](Src/hover_uart_driver.h) — serial protocol to the STM32 hoverboard controller

### STM32 layer

The STM32 side lives in [`Src/main.c`](Src/main.c) and related low-level files in [`Src/`](Src). It is based on hoverboard motor control firmware extended for gate mode:

- receives speed commands over UART
- returns ASCII telemetry lines (`TEL,...`)
- exposes RPM, distance, fault, current and optional hall diagnostics
- applies low-level motor PWM / commutation handling

Important STM32 files:

- [`Src/main.c`](Src/main.c) — main gate-mode runtime and telemetry output
- [`Src/util.c`](Src/util.c) — UART processing and support logic
- [`Src/setup.c`](Src/setup.c) — peripheral initialization
- [`Src/comms.c`](Src/comms.c) — data exposure / debug variables
- [`Src/control.c`](Src/control.c), [`Src/bldc.c`](Src/bldc.c), [`Src/BLDC_controller.c`](Src/BLDC_controller.c) — motor control stack

## ESP32 ↔ STM32 UART communication

The UART link is encapsulated in [`HoverUartDriver`](Src/hover_uart_driver.h). The ESP32 sends commands and receives telemetry lines such as:

- `dir`
- `rpm`
- `dist_mm`
- `fault`
- `bat_cV`
- `iA`
- `armed`
- optional hall diagnostic fields

On the ESP32, telemetry is used for:

- motion supervision
- homing start conditions
- timeout / offline recovery
- position tracking
- current limiting and safety decisions

## Main features

### Gate control

- semantic OPEN / CLOSE / STOP control via [`GateController`](Src/gate_controller.cpp)
- physical motor direction abstraction via [`MotorController`](Src/motor_controller.cpp)
- consistent logical semantics independent of low-level motor sign

### Startup homing and position reference

- startup reference from CLOSE limit when available
- temporary helper position when position is uncertain after reboot
- automatic homing toward CLOSE
- resync to zero after reaching CLOSE
- retry path when telemetry is late after reboot

### Safety

- limit switch handling
- photocell / obstacle input
- watchdog integration
- current limit and cooldown logic
- telemetry timeout and offline detection
- motion block during OTA

### Recovery

- hoverboard telemetry recovery path
- startup retry when telemetry is delayed after boot
- motion profile restore after homing or failure

### LED ring

- WS2812B status signalling
- learn mode indication
- startup / override / network / motion visual states

### Remotes and learn mode

- HCS301 remote support
- anti-replay / anti-repeat logic
- persistence of learned remotes

### Web interface and API

Static UI in [`data/`](data) with pages for:

- dashboard
- settings
- calibration
- remotes
- sensors

The web stack provides:

- `/api/status`
- `/api/status-lite`
- `/api/diagnostics`
- `/api/control`
- calibration endpoints
- WebSocket push status via `/ws`

### MQTT / Home Assistant style integration

MQTT support is available through [`Src/mqtt_manager.cpp`](Src/mqtt_manager.cpp). It is optional and disabled by default in the sample config.

## Direction logic status

Current runtime direction architecture:

- one ESP32 runtime source of truth: `motor.invertDir`
- no runtime OR with `gpio.dirInvert`
- calibration confirmation applies exactly the chosen invert state
- calibration UI performs a real toggle of the current state
- OPEN / CLOSE remain semantic gate commands, not raw motor-sign commands

Legacy flat layout has been reorganized into dedicated [`esp32/`](esp32) and [`stm32/`](stm32) directories.

## Build instructions

### ESP32 build

```bash
python -m platformio run -e esp32
```

### ESP32 upload over COM3

```bash
python -m platformio run -e esp32 -t upload --upload-port COM3
python -m platformio run -e esp32 -t uploadfs --upload-port COM3
```

Optional flash erase before upload:

```bash
python -m platformio run -e esp32 -t erase --upload-port COM3
```

### ESP32 OTA upload

Update the placeholders in [`platformio.ini`](platformio.ini):

- `upload_port = CHANGE_DEVICE_IP`
- `--auth=CHANGE_OTA_PASSWORD`

Then run:

```bash
python -m platformio run -e esp32_ota -t upload
python -m platformio run -e esp32_ota -t uploadfs
```

### STM32 build

The STM32 source tree is stored in [`Src/`](Src). Build method depends on the STM32 toolchain/project setup used on the local machine. At minimum, the source set includes [`Src/main.c`](Src/main.c), [`Src/setup.c`](Src/setup.c), [`Src/util.c`](Src/util.c) and the BLDC control files.

### STM32 upload with ST-Link

Use the STM32 build output produced by the local STM32 IDE / build system and flash it with ST-Link. The exact command depends on the chosen STM32 tooling and target board configuration.

## Tests and diagnostics

The [`scripts/`](scripts) directory contains regression and stress tooling, including:

- API / serial smoke tests
- position accuracy and reliability tests
- motor focus tests
- LED logic tests
- remotes persistence tests
- OTA smoke tests

Examples:

```bash
python scripts/ota_smoke_test.py --host CHANGE_DEVICE_IP --expect-ota-enabled
python scripts/run_position_accuracy_test.py
python scripts/run_position_reliability_test.py
python scripts/test_led_logic.py
```

## Security and privacy notes

- This repository should not store real Wi-Fi passwords, OTA passwords, MQTT credentials or production API tokens.
- [`data/config.json`](data/config.json) is intended as a sanitized sample config.
- local reports, serial captures and generated test outputs should not be committed.

## Known limitations

- final physical direction still depends on the actual motor wiring and STM32 firmware build variant
- startup homing requires usable telemetry and/or limit signals
- STM32 build and flash flow is not yet packaged as a fully reproducible PlatformIO environment in this repo
- hardware-specific tuning values still require per-install calibration

## Current project status

- active firmware development
- startup homing and telemetry recovery logic present
- direction logic recently unified on ESP32 runtime
- web UI, diagnostics and automated scripts available
- hardware validation remains essential for each installation

## Additional documentation

- [`ARCHITEKTURA.md`](ARCHITEKTURA.md)
- [`API.md`](API.md)
- [`TESTY.md`](TESTY.md)
- [`HOMING_RECOVERY.md`](HOMING_RECOVERY.md)
- [`LED_LOGIC.md`](LED_LOGIC.md)
- [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md)
- recent changes in [`CHANGELOG.md`](CHANGELOG.md)


# Architecture

## Overview

The system is split into a high-level controller on [`ESP32`](Src/app_main.cpp) and a low-level motor controller on [`STM32`](Src/main.c).

## Main runtime layers

1. Configuration and persistence — [`ConfigManager`](Src/config_manager.cpp)
2. Motor abstraction — [`MotorController`](Src/motor_controller.cpp)
3. Gate semantics — [`GateController`](Src/gate_controller.cpp)
4. Position and homing — [`PositionTracker`](Src/position_tracker.cpp) and startup homing in [`Src/app_main.cpp`](Src/app_main.cpp)
5. Web/API/MQTT/OTA — [`Src/web_server.cpp`](Src/web_server.cpp), [`Src/mqtt_manager.cpp`](Src/mqtt_manager.cpp), OTA logic in [`Src/app_main.cpp`](Src/app_main.cpp)

## Direction model

- semantic OPEN/CLOSE belongs to [`GateController`](Src/gate_controller.cpp)
- runtime physical inversion belongs to `motor.invertDir`
- calibration modifies `motor.invertDir`
- `gpio.dirInvert` is legacy-only and should not change runtime direction

## UART telemetry flow

- commands from ESP32 to STM32: speed / arm / disarm / zero / get
- telemetry from STM32 to ESP32: `TEL,...`
- parsed in [`HoverUartDriver::handleRx()`](Src/hover_uart_driver.cpp:269)

## Startup homing flow

- determine whether OPEN or CLOSE limit already gives a reference
- if position is uncertain, expose temporary helper state
- wait for startup telemetry health
- move toward OPEN using reduced profile when neither limit is active
- resync to `0 m` on CLOSE or to `maxDistance` on OPEN

## Safety paths

- obstacle / photocell
- current limiting
- telemetry timeout
- hover fault handling
- OTA motion blocking

# Known Issues

## Hardware-specific direction dependency

Even with unified ESP32 runtime direction logic, final physical behavior still depends on motor wiring and STM32 firmware build assumptions.

## Telemetry dependency at startup

Startup homing depends on valid telemetry and/or limit switch state. If the STM32 boots slowly, startup may remain pending until telemetry is available.

## Local STM32 build reproducibility

The STM32 source is included, but the exact IDE/toolchain project used to build the target binary may differ between installations.

## Test scripts are installation-specific

Many scripts assume a concrete local API base URL, serial port or hardware setup and may need adjustment before use.

# STM32 Hoverboard Controller

This directory contains the low-level STM32 hoverboard motor controller firmware used by GateOS.

## Contents

- [`stm32/Src/main.c`](stm32/Src/main.c) — gate-mode runtime, telemetry and motor command handling
- [`stm32/Src/util.c`](stm32/Src/util.c) — UART processing and helpers
- [`stm32/Src/setup.c`](stm32/Src/setup.c) — MCU peripheral setup
- BLDC control files in [`stm32/Src/`](stm32/Src)

## Responsibilities

- receive motor commands from ESP32 over UART
- return telemetry (`TEL,...`)
- run BLDC / hoverboard low-level motor control

## Build / flash

STM32 build depends on the local STM32 toolchain/project setup used for this firmware source tree.
Flash with ST-Link using the binary produced by the STM32 build environment.

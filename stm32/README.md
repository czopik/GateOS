# STM32 Hoverboard Controller

This directory contains the low-level STM32 hoverboard motor controller firmware used by GateOS.

## Contents

### Core Files
- [`Src/main.c`](Src/main.c) — Main loop, gate-mode runtime, telemetry handling
- [`Src/util.c`](Src/util.c) — UART processing, command parsing, helpers
- [`Src/control.c`](Src/control.c) — Input control (PPM, PWM, Nunchuk)
- [`Src/setup.c`](Src/setup.c) — MCU peripheral initialization
- [`Src/bldc.c`](Src/bldc.c) — BLDC motor driver
- [`Src/BLDC_controller.c`](Src/BLDC_controller.c) — Auto-generated motor control

### GateOS Additions (Src/gate_app/)
- [`gate_app/uart_protocol.h`](Src/gate_app/uart_protocol.h) — Protocol definitions, CRC16, frame structures
- [`gate_app/uart_protocol.c`](Src/gate_app/uart_protocol.c) — Protocol implementation
- [`gate_app/gate_controller.h`](Src/gate_app/gate_controller.h) — Gate state machine API
- [`gate_app/gate_controller.c`](Src/gate_app/gate_controller.c) — Safe gate movement control

## Responsibilities

- Receive motor commands from ESP32 over UART (binary + ASCII protocol)
- Return telemetry (`TEL,...` format) at configurable rate
- Run BLDC / hoverboard low-level motor control
- Implement safety features:
  - Soft start/stop ramps
  - Direction reversal protection
  - Zero-cross detection
  - Emergency braking
  - Connection timeout monitoring

## Architecture

```
┌─────────────┐     UART      ┌─────────────┐
│   ESP32     │◄─────────────►│   STM32     │
│  (GateOS)   │  Binary+ASCII │  (Motor Ctrl)│
└─────────────┘               └─────────────┘
                                   │
                            ┌──────┴──────┐
                            │             │
                       Left Motor    Right Motor
```

## Protocol

### Binary Frames (low latency commands)
- Start byte: 0xAA
- Message type + payload
- CRC16-CCITT checksum

### ASCII Commands (debugging/config)
- `ARM` / `DISARM` — Enable/disable motor output
- `GET` — Request telemetry update
- `ZERO` — Reset position counter
- `MODE SPD|TRQ|VLT` — Set control mode
- `HELP` — List available commands

### Telemetry Format
```
TEL,dir=<d>,rpm=<r>,dist_mm=<d>,fault=<f>,bat_cV=<v>,iA=<c>,armed=<a>,mode=<m>,eb=<e>,cmd_age_ms=<t>
```

## Build / Flash

STM32 build depends on the local STM32 toolchain/project setup used for this firmware source tree.

### Using STM32CubeIDE
1. Import project into STM32CubeIDE
2. Select appropriate target board (STM32F103)
3. Build project (Ctrl+B)
4. Flash using ST-Link debugger

### Using Command Line (arm-none-eabi-gcc)
```bash
# Set toolchain path
export TOOLCHAIN_PATH=/path/to/gcc-arm-none-eabi/bin

# Build
make -f Makefile.gateos

# Flash with st-flash
st-flash write build/GateOS.bin 0x8000000
```

## Configuration

Edit `config.h` to enable GateOS variant:
```c
#define VARIANT_USART           // Enable UART communication with ESP32
#define FEEDBACK_SERIAL_USART2  // Use USART2 for telemetry output
// #define CONTROL_SERIAL_USART2 // Use USART2 for command input (bidirectional)
```

## Pinout (Typical)

| Signal | STM32 Pin | ESP32 Pin |
|--------|-----------|-----------|
| UART TX | PA2 (USART2) | GPIO17 |
| UART RX | PA3 (USART2) | GPIO16 |
| GND | GND | GND |

## Safety Notes

⚠️ **WARNING**: This firmware controls high-power motors. Ensure:
- Proper emergency stop mechanism is in place
- Limit switches are correctly wired and tested
- Photocell/safety sensors are functional before operation
- Motor power is disconnected during firmware updates

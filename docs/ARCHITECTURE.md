# GateOS Architecture Documentation

## Overview

GateOS is a production-ready sliding gate controller system with dual-processor architecture:
- **ESP32**: Main controller (logic, web UI, API, connectivity)
- **STM32**: Motor controller (hoverboard firmware hack, FOC motor control)

## System Goals

1. **Stable**: No freezes, no random stops, deterministic behavior
2. **Safe**: Multiple safety layers, fault detection, emergency stop
3. **Modular**: Clean separation of concerns, testable components
4. **Maintainable**: Clear documentation, automated tests

---

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                          ESP32 (Main Controller)                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │
│  │   Web UI     │  │    MQTT      │  │  REST API    │              │
│  │  (WebSocket) │  │   Manager    │  │   Server     │              │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘              │
│         │                 │                  │                       │
│         └─────────────────┼──────────────────┘                       │
│                           │                                          │
│                  ┌────────▼────────┐                                │
│                  │  GateController │ ← State Machine                │
│                  │   (State Logic) │                                 │
│                  └────────┬────────┘                                │
│                           │                                          │
│    ┌──────────────────────┼──────────────────────┐                 │
│    │                      │                      │                  │
│    ▼                      ▼                      ▼                  │
│ ┌──────────┐      ┌──────────────┐      ┌──────────────┐          │
│ │  Safety  │      │   Position   │      │   Motor      │          │
│ │ Manager  │      │   Tracker    │      │ Controller   │          │
│ └────┬─────┘      └──────┬───────┘      └──────┬───────┘          │
│      │                   │                      │                  │
│      │            ┌──────▼───────┐      ┌──────▼───────┐          │
│      │            │ Hall/Encoder │      │ UART Manager │          │
│      │            │   Sensors    │      │ (Queue-based)│          │
│      │            └──────────────┘      └──────┬───────┘          │
│      │                                         │                  │
│      └─────────────────────────────────────────┘                  │
│                                                                   │
│  FreeRTOS Tasks:                                                  │
│  - RX Task (Priority 5): UART reception                           │
│  - Parser Task (Priority 4): Frame validation                     │
│  - TX Task (Priority 3): Command queue processing                 │
│  - Web Server (Priority 2): HTTP/WebSocket                        │
│  - Main Loop (Priority 1): Gate logic                             │
└─────────────────────────────────────────────────────────────────────┘
                              │
                    UART (ASCII/Binary)
                    Baud: 115200 or 9600
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     STM32 (Motor Controller)                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │
│  │   BLDC       │  │   FOC/SIN    │  │  Telemetry   │              │
│  │  Controller  │  │   Control    │  │  Generator   │              │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘              │
│         │                 │                  │                       │
│         └─────────────────┼──────────────────┘                       │
│                           │                                          │
│                  ┌────────▼────────┐                                │
│                  │  Motor Driver   │ ← PWM/FOC                      │
│                  │   (3-phase)     │                                 │
│                  └────────┬────────┘                                │
│                           │                                          │
│         ┌─────────────────┼─────────────────┐                       │
│         │                 │                 │                        │
│         ▼                 ▼                 ▼                        │
│   ┌──────────┐    ┌──────────┐    ┌──────────────┐                 │
│   │  Motor   │    │  Hall    │    │ Current/Volt │                 │
│   │ (BLDC)   │    │ Sensors  │    │  Sensing     │                 │
│   └──────────┘    └──────────┘    └──────────────┘                 │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Module Responsibilities

### ESP32 Modules

#### 1. UartManager (`drivers/uart_manager.h/cpp`)
**Purpose**: Reliable bidirectional communication with STM32

**Features**:
- FreeRTOS task-based architecture (RX, Parser, TX)
- Queue-based command interface (thread-safe)
- ASCII telemetry parsing (TEL,... format)
- Connection monitoring with timeout detection
- CRC validation (for binary frames)
- Keepalive mechanism

**API**:
```cpp
UartManager uart;
uart.begin(rxPin, txPin, baud);
uart.sendSpeedCommand(speed);
uart.sendArmCommand();
uart.isTelemetryValid(timeoutMs);
```

#### 2. SafetyManager (`safety/safety_manager.h/cpp`)
**Purpose**: Centralized safety monitoring and fault management

**Features**:
- Photocell/obstacle detection with debouncing
- Limit switch monitoring (open/close)
- Emergency stop handling
- Over-current protection
- Watchdog monitoring
- Fault state machine with latching

**Safety Chain**:
```
Raw Input → Debounce → Edge Detect → Event Dispatch → Fault Logic
```

**Fault Types**:
- `SAFETY_FAULT_OBSTACLE`
- `SAFETY_FAULT_LIMIT_INVALID`
- `SAFETY_FAULT_TELEMETRY_LOST`
- `SAFETY_FAULT_MOTOR_FAULT`
- `SAFETY_FAULT_OVER_CURRENT`
- `SAFETY_FAULT_WATCHDOG`

#### 3. PositionTracker (`position_tracker.h/cpp`)
**Purpose**: Accurate gate position tracking with multiple sensor sources

**Features**:
- Dual-source positioning (Hall encoder OR hoverboard telemetry)
- Limit switch resync
- Persistent position storage (LittleFS)
- CRC-protected snapshots
- Drift correction

**Position Sources**:
1. **Hall Encoder**: Direct pulse counting via ISR
2. **Hoverboard Telemetry**: Distance from STM32 motor controller

#### 4. GateController (`gate_controller.h/cpp`)
**Purpose**: High-level gate state machine

**States**:
- `GATE_STOPPED`: Gate is stationary
- `GATE_OPENING`: Moving toward open limit
- `GATE_CLOSING`: Moving toward close limit
- `GATE_ERROR`: Fault state, requires intervention

**State Transitions**:
```
STOPPED ──[OPEN CMD]──> OPENING ──[LIMIT/STOP]──> STOPPED
STOPPED ──[CLOSE CMD]─> CLOSING ──[LIMIT/STOP]──> STOPPED
ANY     ──[FAULT]─────> ERROR   ──[CLEAR]───────> STOPPED
```

#### 5. MotorController (`motor_controller.h/cpp`)
**Purpose**: Motor abstraction layer

**Modes**:
- **PWM/DIR**: Traditional GPIO control
- **Hover UART**: Serial command to STM32

**Features**:
- Soft start/stop ramping
- Direction control with inversion
- Motion profiling (accel/cruise/decel)
- Target distance tracking

#### 6. WebServerManager (`web_server.h/cpp`)
**Purpose**: HTTP API and WebSocket interface

**Features**:
- Non-blocking request handling
- Async WebSocket updates
- Minimal JSON payloads
- OTA update support

---

### STM32 Modules

#### 1. BLDC_Controller (`BLDC_controller.c`)
Auto-generated motor control algorithm (Matlab/Simulink)
- FOC (Field Oriented Control)
- Sinusoidal commutation
- Speed/torque control loops

#### 2. Comms (`comms.c`)
Serial communication protocol
- ASCII command parsing (ARM, DISARM, ZERO, GET)
- Telemetry generation (TEL,...)
- Parameter read/write

#### 3. Main Loop (`main.c`)
Real-time motor control
- 10kHz PWM loop
- Current sensing
- Hall sensor decoding
- UART telemetry output

---

## Data Flow

### Command Flow (ESP32 → STM32)
```
User Input (Web/API)
    │
    ▼
GateController.handleCommand()
    │
    ▼
MotorController.setTargetSpeed()
    │
    ▼
UartManager.sendSpeedCommand()  [TX Queue]
    │
    ▼
UART TX Task → Serial2.print()
    │
    ▼
STM32 UART RX → Command Parser
    │
    ▼
Motor Speed Update
```

### Telemetry Flow (STM32 → ESP32)
```
STM32 Motor Control Loop
    │
    ▼
Telemetry Generation (TEL,dir=X,rpm=Y,dist_mm=Z,...)
    │
    ▼
UART TX → ESP32 RX
    │
    ▼
UartManager RX Task → handleRxByte()
    │
    ▼
Parser → parseTelemetry()
    │
    ▼
Callback → PositionTracker.update()
    │
    ▼
WebSocket Broadcast
```

---

## Timing Analysis

| Task | Priority | Period | Max Execution | Stack |
|------|----------|--------|---------------|-------|
| UART RX | 5 | 5ms | <1ms | 4KB |
| UART Parser | 4 | 10ms | <2ms | 4KB |
| UART TX | 3 | 20ms | <1ms | 3KB |
| Safety Update | 2 | 10ms | <0.5ms | - |
| Gate Logic | 1 | 50ms | <5ms | - |
| Web Server | 2 | Event | <10ms | 8KB |

**Watchdog**: 5 second timeout, fed every loop iteration

---

## Memory Layout

### ESP32
- **Flash**: 4MB (LittleFS: ~1.5MB, Code: ~2MB)
- **PSRAM**: Optional 4MB for large buffers
- **Heap**: ~300KB available
- **Stack per task**: 3-8KB

### STM32
- **Flash**: 128KB-512KB (depending on variant)
- **SRAM**: 20KB-64KB
- **EEPROM emulation**: Flash-based

---

## Error Handling

### Fault Recovery Matrix

| Fault | Auto-Recover | Manual Reset | Notes |
|-------|-------------|--------------|-------|
| Obstacle | Yes (when cleared) | No | Requires obstacle removal |
| Limit Invalid | No | Yes | Check wiring |
| Telemetry Lost | Yes (when restored) | No | Timeout = 1000ms |
| Motor Fault | No | Yes | Check STM32 diagnostics |
| Over-Current | Cooldown then retry | If repeated | Threshold = 10A |
| Watchdog | No | Yes | Software restart |

### Diagnostic Output
```
[SAFETY] Fault set: OVER_CURRENT
[SAFETY] Fault cleared: OVER_CURRENT
[UART] Connection lost
[UART] Connection restored
[POS] snapshot loaded pos=2.500m max=5.000m
```

---

## Testing Strategy

### Unit Tests
- Position calculation accuracy
- State machine transitions
- Debounce timing
- CRC validation

### Integration Tests
- Open/close cycle repeatability
- UART stress test (1000+ commands)
- Fault injection recovery
- Power loss recovery

### Field Tests
- Temperature cycling (-10°C to +50°C)
- Voltage variation (10V-15V)
- EMI/EMC immunity
- Long-term reliability (10K+ cycles)

---

## Configuration

### Key Parameters (config.json)
```json
{
  "gate": {
    "maxDistance": 5.0,
    "position": 0.0,
    "wheelCircumference": 0.15,
    "pulsesPerRevolution": 12,
    "telemetryTimeoutMs": 1000
  },
  "sensors": {
    "photocell": {
      "enabled": true,
      "pin": 25,
      "debounceMs": 30
    },
    "hall": {
      "enabled": true,
      "pin": 26,
      "debounceMs": 1
    }
  },
  "motor": {
    "overCurrentThreshold": 10.0,
    "overCurrentDurationMs": 500
  }
}
```

---

## Future Improvements

1. **Binary UART Protocol**: More efficient than ASCII
2. **Dual Motor Support**: For larger gates
3. **Battery Backup**: UPS integration
4. **Camera Integration**: Visual verification
5. **Machine Learning**: Anomaly detection for motor health
6. **OTA for STM32**: Remote firmware updates

---

## Revision History

| Version | Date | Changes |
|---------|------|---------|
| 2.0 | 2024 | Complete refactor with FreeRTOS tasks |
| 1.5 | 2023 | Added safety manager module |
| 1.0 | 2022 | Initial release |

# GateOS API Documentation

## REST API Endpoints

### Base URL
```
http://gate.local/api
or
http://<IP_ADDRESS>/api
```

---

## Control Endpoints

### POST /api/control

Send gate control commands.

**Request:**
```json
{
  "action": "open|close|stop|toggle"
}
```

Alternative shorthand is also accepted:

```json
{
  "position": 1.25
}
```

**Response:**
```json
{
  "status": "ok"
}
```

**Actions:**
- `open` - Start opening gate
- `close` - Start closing gate  
- `stop` - Stop movement immediately
- `toggle` - Toggle between open/close
- `position` / `target` - Move to a position in meters
- `positionMm` / `targetMm` - Move to a position in millimeters

---

### POST /api/move

Move to specific position.

**Request:**
```json
{
  "position": 2.5
}
```

**Parameters:**
- `position` (float): Target position in meters from closed limit

**Response:**
```json
{
  "status": "ok"
}
```

---

### POST /api/gate/calibrate

Calibration commands.

**Request:**
```json
{
  "set": "zero|max|reset"
}
```

**Modes:**
- `zero` - Set current position as closed (0m)
- `max` - Set current position as open (max distance)
- `reset` - Reset calibration data

**Response:**
```json
{
  "status": "ok"
}
```

---

## Status Endpoints

### GET /api/status

Get complete gate status.

**Response:**
```json
{
  "uptimeMs": 123456,
  "runtime": {
    "freeHeap": 201344,
    "wsClients": 1
  },
  "gate": {
    "state": "stopped",
    "moving": false,
    "position": 2.45,
    "positionPercent": 49,
    "targetPosition": 2.45,
    "maxDistance": 5.0,
    "errorCode": 0,
    "stopReason": 8,
    "faultSeverityCode": 2,
    "faultSeverity": "soft_fault",
    "faultCode": 4,
    "faultReason": 8,
    "warningCount": 1,
    "softFaultCount": 3,
    "obstacle": false,
    "lastStateChangeMs": 123400,
    "lastMoveMs": 123420
  },
  "wifi": {
    "connected": true,
    "mode": "STA",
    "ip": "192.168.1.44"
  },
  "mqtt": {
    "connected": false
  },
  "hb": {
    "enabled": true,
    "rpm": 0,
    "iA": 0.0,
    "fault": 0,
    "telAgeMs": 42
  },
  "inputs": {
    "limitOpen": false,
    "limitClose": false,
    "obstacle": false
  }
}
```

**Important note:** operational gate state now lives under `gate.*`. Top-level status is a nested snapshot of runtime, gate, telemetry and IO.

**Gate fields:**
- `gate.state` - `stopped`, `opening`, `closing`, `error`
- `gate.moving` - boolean movement flag
- `gate.errorCode` - legacy error code; fatal faults still drive `state="error"`
- `gate.stopReason` - numeric `GateStopReason`
- `gate.faultSeverity` - `none`, `warning`, `soft_fault`, `fatal_fault`
- `gate.faultCode` - numeric `GateErrorCode` associated with the active fault/warning
- `gate.faultReason` - numeric `GateStopReason` associated with the active fault/warning
- `gate.warningCount` - accumulated warning count since boot
- `gate.softFaultCount` - accumulated soft-fault count since boot

**Fault severity semantics:**
- `warning` - non-fatal condition; UI/automation may continue issuing movement commands
- `soft_fault` - gate stopped because of a recoverable runtime condition; UI/automation may continue issuing a new movement command
- `fatal_fault` - movement should remain blocked until the fatal condition is cleared

### GET /api/status-lite

Get lightweight status for fast polling.

**Response:**
```json
{
  "state": "stopped",
  "moving": false,
  "positionMm": 2450,
  "positionPercent": 49,
  "errorCode": 0,
  "faultSeverity": "soft_fault",
  "faultCode": 4,
  "faultReason": 8,
  "warningCount": 1,
  "softFaultCount": 3,
  "limitOpen": false,
  "limitClose": false,
  "rpm": 0,
  "iA": 0.0
}
```

`/api/status-lite` mirrors the ETAP 1 fault model from full status and is the recommended polling endpoint for dashboards that only need live state plus fault severity.

---

### GET /api/diagnostics

Get system diagnostics.

**Response:**
```json
{
  "uptime": 3600,
  "freeHeap": 150000,
  "wifiRSSI": -65,
  "uartStats": {
    "rxLines": 125000,
    "txCommands": 5000,
    "badLines": 12,
    "errorRate": 0.01
  },
  "motor": {
    "rpm": 0,
    "current": 0.5,
    "voltage": 12.6,
    "temperature": 35,
    "fault": false
  },
  "sensors": {
    "limitOpen": false,
    "limitClose": false,
    "photocell": false,
    "hallAttached": true
  },
  "resetReason": "POWER_ON"
}
```

---

## Configuration Endpoints

### GET /api/config

Get current configuration.

**Response:**
```json
{
  "gate": {
    "maxDistance": 5.0,
    "position": 2.45,
    "wheelCircumference": 0.15,
    "pulsesPerRevolution": 12
  },
  "sensors": {
    "photocell": {
      "enabled": true,
      "pin": 25,
      "debounceMs": 30,
      "invert": false
    },
    "hall": {
      "enabled": true,
      "pin": 26,
      "debounceMs": 1
    }
  },
  "motor": {
    "maxSpeed": 100,
    "acceleration": 50,
    "overCurrentThreshold": 10.0
  }
}
```

---

### POST /api/config or PUT /api/config

Update and persist configuration. `PUT` is accepted as a REST-compatible alias for the existing `POST` endpoint.

**Request:**
```json
{
  "gate": {
    "maxDistance": 5.5
  }
}
```

**Response:**
```json
{
  "status": "ok",
  "apply": "scheduled"
}
```

---

### POST /api/factory_reset or POST /api/factory-reset

Reset to factory defaults. Both spellings are accepted; `/api/factory_reset` is the original endpoint and `/api/factory-reset` is a compatibility alias.

**Request:**
```json
{
  "confirm": true
}
```

**Response:**
```json
{
  "status": "ok"
}
```

---

## WebSocket API

### Connection

```
ws://gate.local/ws
```

### Subscribe to Updates

Send after connecting:
```json
{
  "action": "subscribe",
  "events": ["status", "position", "diagnostics"]
}
```

### Events Received

**Status Update:**
```json
{
  "type": "status",
  "data": {
    "state": "OPENING",
    "position": 1.25,
    "moving": true
  }
}
```

**Position Update:**
```json
{
  "type": "position",
  "data": {
    "position": 1.30,
    "percent": 26
  }
}
```

**Safety Event:**
```json
{
  "type": "safety",
  "data": {
    "event": "OBSTACLE_DETECTED",
    "timestamp": 1699123456789
  }
}
```

---

## Error Responses

### Standard Error Format

```json
{
  "success": false,
  "error": {
    "code": "INVALID_STATE",
    "message": "Cannot open: obstacle detected",
    "details": {
      "currentState": "STOPPED",
      "obstacle": true
    }
  }
}
```

### Error Codes

| Code | HTTP Status | Description |
|------|-------------|-------------|
| `INVALID_STATE` | 400 | Command invalid for current state |
| `NOT_FOUND` | 404 | Endpoint not found |
| `UNAUTHORIZED` | 401 | Authentication required |
| `TIMEOUT` | 504 | Operation timeout |
| `INTERNAL_ERROR` | 500 | Internal server error |

---


## Security and limits

Critical endpoints such as config, remotes, diagnostics, filesystem status/list, OTA upload, reboot and factory reset require the configured API token whenever one exists. This remains true even if general API security is disabled for basic compatibility.

JSON request bodies are capped to protect ESP32 heap:

- 16 KiB for `/api/config`, `/api/config/validate` and `/api/motion/profile`
- 4 KiB for other JSON API endpoints

If a response document overflows its ArduinoJson capacity, the API returns:

```json
{
  "status": "error",
  "error": "json_overflow"
}
```

---

## Rate Limits

- REST API: 10 requests/second
- WebSocket: 100 messages/second
- Control commands: 1 command/second (debounced)

---

## Examples

### cURL Examples

**Open gate:**
```bash
curl -X POST http://gate.local/api/control \
  -H "Content-Type: application/json" \
  -d '{"action": "open"}'
```

**Get status:**
```bash
curl http://gate.local/api/status
```

**Move to position:**
```bash
curl -X POST http://gate.local/api/move \
  -H "Content-Type: application/json" \
  -d '{"position": 3.0}'
```

**Subscribe via WebSocket (using wscat):**
```bash
wscat -c ws://gate.local/ws
> {"action": "subscribe", "events": ["status"]}
```

### Python Example

```python
import requests

BASE_URL = "http://gate.local/api"

def open_gate():
    resp = requests.post(f"{BASE_URL}/control", 
                        json={"action": "open"})
    return resp.json()

def get_position():
    resp = requests.get(f"{BASE_URL}/status-lite")
    data = resp.json()
    return data.get("positionMm")

def wait_for_stop(timeout=60):
    import time
    start = time.time()
    while time.time() - start < timeout:
        status = requests.get(f"{BASE_URL}/status-lite").json()
        if not status["moving"]:
            return status.get("positionMm")
        time.sleep(0.1)
    return None
```

---

## Firmware Version

### GET /api/version

**Response:**
```json
{
  "version": "2.0.0",
  "buildDate": "2024-01-15",
  "gitHash": "abc123",
  "platform": "ESP32",
  "features": ["websocket", "mqtt", "ota"]
}
```

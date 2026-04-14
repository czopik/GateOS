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

### POST /api/calibrate

Calibration commands.

**Request:**
```json
{
  "mode": "zero|max|reset"
}
```

**Modes:**
- `zero` - Set current position as closed (0m)
- `max` - Set current position as open (max distance)
- `reset` - Reset calibration data

**Response:**
```json
{
  "success": true,
  "mode": "zero",
  "position": 0.0
}
```

---

## Status Endpoints

### GET /api/status

Get complete gate status.

**Response:**
```json
{
  "state": "STOPPED",
  "moving": false,
  "position": 2.45,
  "positionPercent": 49,
  "maxDistance": 5.0,
  "targetPosition": 2.45,
  "obstacle": false,
  "wifiConnected": true,
  "mqttConnected": false,
  "apMode": false,
  "lastStateChangeMs": 1699123456789,
  "error": null,
  "stopReason": "NONE"
}
```

**States:**
- `STOPPED` - Gate is stationary
- `OPENING` - Moving toward open limit
- `CLOSING` - Moving toward close limit
- `ERROR` - Fault state

**Error Codes:**
- `null` - No error
- `"TIMEOUT"` - Telemetry timeout
- `"OBSTACLE"` - Obstacle detected
- `"HOVER_FAULT"` - Motor controller fault
- `"LIMITS_INVALID"` - Both limits triggered
- `"OVER_CURRENT"` - Motor over-current

---

### GET /api/position

Get current position only.

**Response:**
```json
{
  "position": 2.45,
  "maxDistance": 5.0,
  "percent": 49,
  "unit": "meters"
}
```

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

### GET /api/safety

Get safety system status.

**Response:**
```json
{
  "safeToMove": true,
  "fault": null,
  "obstacleActive": false,
  "limitsValid": true,
  "watchdogOk": true,
  "overCurrentLast": 0.0,
  "faultCount": 0
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

### PUT /api/config

Update configuration.

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
  "success": true,
  "restartRequired": false
}
```

---

### POST /api/config/save

Save current configuration to flash.

**Response:**
```json
{
  "success": true,
  "message": "Configuration saved"
}
```

---

### POST /api/factory-reset

Reset to factory defaults.

**Request:**
```json
{
  "confirm": true
}
```

**Response:**
```json
{
  "success": true,
  "message": "Factory reset scheduled. Device will restart."
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
    resp = requests.get(f"{BASE_URL}/position")
    return resp.json()["position"]

def wait_for_stop(timeout=60):
    import time
    start = time.time()
    while time.time() - start < timeout:
        status = requests.get(f"{BASE_URL}/status").json()
        if not status["moving"]:
            return status["position"]
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

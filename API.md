# API

## Main endpoints

- `GET /api/status` — full runtime snapshot
- `GET /api/status-lite` — reduced status for lightweight polling
- `GET /api/diagnostics` — diagnostics and counters
- `POST /api/control` — OPEN / CLOSE / STOP / TOGGLE actions
- `GET /ws` — WebSocket status/events

## Calibration endpoints

- `GET /api/calibration/status`
- `GET /api/calibration/manual/status`
- `POST /api/calibration/manual/start`
- `POST /api/calibration/confirm_dir`
- `POST /api/calibration/manual/apply`
- `POST /api/calibration/manual/cancel`

## Status payload highlights

Common fields include:

- gate state and stop reason
- position in meters / mm / percent
- hover telemetry snapshot
- limit / obstacle inputs
- homing state
- OTA status

## Authentication

API token support is implemented in [`Src/web_server.cpp`](Src/web_server.cpp). Keep production tokens outside the repository.

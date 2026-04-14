# Homing and Recovery

## Startup homing

Implemented mainly in [`runStartupHoming()`](Src/app_main.cpp:468).

### Goals

- accept an already-active limit as a trusted startup reference
- establish a reliable OPEN reference after reboot when neither limit is active
- avoid false certainty when position is unknown
- allow retry if telemetry starts late

## Temporary helper position

When both limits are inactive and position is uncertain, the runtime exposes a temporary helper distance of 100 mm. This is not treated as a trusted reference.

## Direct startup reference

If a single limit is already active at boot:

- `CLOSE` active -> position is immediately resynced to `0 m`
- `OPEN` active -> position is immediately resynced to `maxDistance`
- no startup homing move is started

## OPEN reference acquisition

If both limits are inactive at boot:

- the runtime exposes a temporary helper position of `100 mm`
- a slow startup homing move is started toward `OPEN`
- after reaching `OPEN`, position becomes certain and is resynced to `maxDistance`

After reaching OPEN:

- control position is resynced
- position becomes certain
- temporary helper state is cleared

## Telemetry recovery

Recent improvement: missing startup telemetry no longer permanently locks startup homing immediately. The controller now stays in a retry/pending path and can continue once telemetry becomes healthy.

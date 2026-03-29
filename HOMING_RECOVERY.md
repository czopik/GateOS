# Homing and Recovery

## Startup homing

Implemented mainly in [`runStartupHoming()`](Src/app_main.cpp:468).

### Goals

- establish a reliable CLOSE reference after reboot
- avoid false certainty when position is unknown
- allow retry if telemetry starts late

## Temporary helper position

When both limits are inactive and position is uncertain, the runtime exposes a temporary helper distance of 100 mm. This is not treated as a trusted reference.

## CLOSE reference acquisition

After reaching CLOSE:

- control position is resynced
- position becomes certain
- temporary helper state is cleared

## Telemetry recovery

Recent improvement: missing startup telemetry no longer permanently locks startup homing immediately. The controller now stays in a retry/pending path and can continue once telemetry becomes healthy.

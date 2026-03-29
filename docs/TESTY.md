# Tests

## Test categories

- API / serial integration
- position accuracy
- position reliability
- stop repeatability
- LED logic
- OTA smoke
- remotes persistence

## Important scripts

- [`scripts/run_api_serial_test.py`](scripts/run_api_serial_test.py)
- [`scripts/run_position_accuracy_test.py`](scripts/run_position_accuracy_test.py)
- [`scripts/run_position_reliability_test.py`](scripts/run_position_reliability_test.py)
- [`scripts/run_stop_repeatability_test.py`](scripts/run_stop_repeatability_test.py)
- [`scripts/run_motor_focus_test.py`](scripts/run_motor_focus_test.py)
- [`scripts/test_led_logic.py`](scripts/test_led_logic.py)
- [`scripts/test_remotes_persistence.py`](scripts/test_remotes_persistence.py)

## Hardware validation checklist

1. startup after reboot
2. OPEN / STOP / CLOSE semantics
3. limit OPEN reaction
4. limit CLOSE reaction and zero resync
5. photocell during CLOSE
6. startup homing retry after late telemetry
7. position increase on OPEN and decrease on CLOSE
8. calibration with zero, single-toggle and double-toggle direction cases

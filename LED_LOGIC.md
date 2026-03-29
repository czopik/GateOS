# LED Logic

LED support is implemented around [`Src/led_controller.cpp`](Src/led_controller.cpp) and configuration in [`data/config.json`](data/config.json).

## Supported roles

- boot indication
- normal status indication
- motion indication
- learn mode indication
- temporary overrides for notable events

## Hardware

- WS2812 / WS2812B style ring
- configurable count, brightness and color order

## Runtime integration

The ESP32 updates LED state according to connectivity, motion, limits, errors and learn mode.

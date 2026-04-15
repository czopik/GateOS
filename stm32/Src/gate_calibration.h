#pragma once

/*
 * GateOS STM32 calibration overrides.
 *
 * Field-calibrated two-point profile for this installation:
 * - Battery only:      multimeter ~35.0V
 * - Charger connected: multimeter ~41.0V
 *
 * CHARGER pin selects calibration profile.
 */

#ifndef GATE_BAT_CALIB_REAL_VOLTAGE_BATTERY
#define GATE_BAT_CALIB_REAL_VOLTAGE_BATTERY 3500
#endif

#ifndef GATE_BAT_CALIB_REAL_VOLTAGE_CHARGING
#define GATE_BAT_CALIB_REAL_VOLTAGE_CHARGING 4100
#endif

#ifndef GATE_CHARGER_ACTIVE_LEVEL
#define GATE_CHARGER_ACTIVE_LEVEL GPIO_PIN_RESET
#endif

/*
 * Keep a default scalar for places that need compile-time init.
 */
#ifndef GATE_BAT_CALIB_REAL_VOLTAGE
#define GATE_BAT_CALIB_REAL_VOLTAGE GATE_BAT_CALIB_REAL_VOLTAGE_BATTERY
#endif

static inline int16_t gate_battery_calibration_cv(void) {
  GPIO_PinState charger = HAL_GPIO_ReadPin(CHARGER_PORT, CHARGER_PIN);
  if (charger == GATE_CHARGER_ACTIVE_LEVEL) {
    return (int16_t)GATE_BAT_CALIB_REAL_VOLTAGE_CHARGING;
  }
  return (int16_t)GATE_BAT_CALIB_REAL_VOLTAGE_BATTERY;
}

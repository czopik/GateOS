#pragma once

/*
 * GateOS STM32 calibration overrides.
 *
 * Gate profile currently targets:
 * - ~35.0V pack at rest
 * - ~41.0V while charger is connected
 *
 * Use 41.00V as calibration anchor (centivolts).
 */

#ifndef GATE_BAT_CALIB_REAL_VOLTAGE
#define GATE_BAT_CALIB_REAL_VOLTAGE 4100
#endif

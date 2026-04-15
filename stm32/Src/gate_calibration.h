#pragma once

/*
 * GateOS STM32 calibration overrides.
 *
 * Field-calibrated for this installation:
 * - STM32 telemetry was reading ~38.25V while multimeter showed 35.0V.
 * - Use a reduced calibration anchor so bat_cV tracks real pack voltage.
 */

#ifndef GATE_BAT_CALIB_REAL_VOLTAGE
#define GATE_BAT_CALIB_REAL_VOLTAGE 3570
#endif

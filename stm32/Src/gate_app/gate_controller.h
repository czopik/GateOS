/**
 * @file gate_controller.h
 * @brief Gate-specific motor control state machine for STM32
 * 
 * This module implements safe gate movement control with:
 * - State machine (STOPPED, OPENING, CLOSING, ERROR)
 * - Soft start/stop ramps
 * - Direction reversal protection
 * - Zero-cross detection
 * - Emergency braking
 */

#ifndef __GATE_CONTROLLER_H__
#define __GATE_CONTROLLER_H__

#include <stdint.h>
#include <stdbool.h>
#include "uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

//==============================================================================
// GATE STATES
//==============================================================================

typedef enum {
    GATE_STATE_STOPPED    = 0x00U,
    GATE_STATE_OPENING    = 0x01U,
    GATE_STATE_CLOSING    = 0x02U,
    GATE_STATE_ERROR      = 0x03U,
    GATE_STATE_CALIBRATING = 0x04U,
} gate_state_t;

typedef enum {
    GATE_CMD_NONE     = 0x00U,
    GATE_CMD_OPEN     = 0x01U,
    GATE_CMD_CLOSE    = 0x02U,
    GATE_CMD_STOP     = 0x03U,
    GATE_CMD_CALIBRATE = 0x04U,
} gate_command_t;

//==============================================================================
// CONFIGURATION
//==============================================================================

#define GATE_RAMP_UP_MS           500U    // Soft start duration
#define GATE_RAMP_DOWN_MS         300U    // Soft stop duration
#define GATE_ZERO_CROSS_RPM       20U     // RPM threshold for direction change
#define GATE_BRAKE_RPM            50U     // RPM threshold for braking
#define GATE_MAX_SPEED_CMD        800     // Max motor command (-1000 to 1000)
#define GATE_MIN_SPEED_CMD        200     // Min motor command for movement

// Reversal guard thresholds
#define GATE_REV_GUARD_RPM        30U     // If reversing above this, hold at 0
#define GATE_REV_ALLOW_RPM        12U     // Allow reverse only below this
#define GATE_REV_BRAKE_CMD        120     // Braking level during reversal guard

//==============================================================================
// PUBLIC API
//==============================================================================

/**
 * @brief Initialize gate controller
 */
void gate_controller_init(void);

/**
 * @brief Process gate state machine (call from main loop)
 * @param current_rpm Current measured RPM (signed)
 * @param current_ms Current timestamp in ms
 */
void gate_controller_process(int16_t current_rpm, uint32_t current_ms);

/**
 * @brief Send gate command
 * @param cmd Command to execute
 * @return true if command accepted
 */
bool gate_controller_send_command(gate_command_t cmd);

/**
 * @brief Set target speed for manual control
 * @param speed Target speed (-1000 to 1000)
 */
void gate_controller_set_speed(int16_t speed);

/**
 * @brief Get current gate state
 * @return Current state
 */
gate_state_t gate_controller_get_state(void);

/**
 * @brief Check if motor is currently armed and enabled
 * @return true if armed
 */
bool gate_controller_is_armed(void);

/**
 * @brief Arm/disarm the motor controller
 * @param armed true to arm, false to disarm
 */
void gate_controller_set_armed(bool armed);

/**
 * @brief Get the current motor commands (left, right)
 * @param cmd_left Pointer to store left motor command
 * @param cmd_right Pointer to store right motor command
 */
void gate_controller_get_motor_commands(int16_t *cmd_left, int16_t *cmd_right);

/**
 * @brief Reset gate controller after error recovery
 */
void gate_controller_reset(void);

/**
 * @brief Check if gate is moving
 * @return true if gate is in motion
 */
bool gate_controller_is_moving(void);

/**
 * @brief Get ramp progress (0-100%)
 * @return Progress percentage
 */
uint8_t gate_controller_get_ramp_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* __GATE_CONTROLLER_H__ */

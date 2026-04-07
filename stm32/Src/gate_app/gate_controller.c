/**
 * @file gate_controller.c
 * @brief Gate-specific motor control state machine implementation
 */

#include "stm32f1xx_hal.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "gate_controller.h"
#include "config.h"
#include "defines.h"
#include "BLDC_controller.h"

//==============================================================================
// MODULE STATE
//==============================================================================

typedef struct {
    gate_state_t state;
    gate_command_t pending_cmd;
    
    int16_t target_speed;       // Target speed (-1000 to 1000)
    int16_t current_speed_cmd;  // Current ramped speed command
    int16_t last_speed_cmd;     // Previous speed command
    
    uint32_t ramp_start_ms;
    uint32_t ramp_duration_ms;
    
    uint8_t armed;
    uint8_t reversal_guard_active;
    
    int16_t cmd_left;
    int16_t cmd_right;
} gate_ctx_t;

static gate_ctx_t g_gate = {0};

//==============================================================================
// PRIVATE FUNCTIONS
//==============================================================================

static void update_ramp(uint32_t current_ms, int16_t current_rpm) {
    uint32_t elapsed = current_ms - g_gate.ramp_start_ms;
    int16_t target = g_gate.target_speed;
    
    // Check if ramp is complete
    if (elapsed >= g_gate.ramp_duration_ms) {
        g_gate.current_speed_cmd = target;
        return;
    }
    
    // Linear ramp calculation
    float progress = (float)elapsed / (float)g_gate.ramp_duration_ms;
    g_gate.current_speed_cmd = (int16_t)((float)g_gate.last_speed_cmd + 
                                         (float)(target - g_gate.last_speed_cmd) * progress);
}

static void check_reversal_guard(int16_t current_rpm) {
    int16_t cmd_sign = (g_gate.target_speed > 0) ? 1 : ((g_gate.target_speed < 0) ? -1 : 0);
    int16_t rpm_sign = (current_rpm > 0) ? 1 : ((current_rpm < 0) ? -1 : 0);
    
    // Reversal detected
    if (cmd_sign != 0 && rpm_sign != 0 && cmd_sign != rpm_sign) {
        if (ABS(current_rpm) > GATE_REV_GUARD_RPM) {
            g_gate.reversal_guard_active = 1;
        } else if (ABS(current_rpm) <= GATE_REV_ALLOW_RPM) {
            g_gate.reversal_guard_active = 0;
        }
    } else {
        g_gate.reversal_guard_active = 0;
    }
}

static void calculate_motor_commands(int16_t current_rpm) {
    int16_t speed_cmd = g_gate.current_speed_cmd;
    
    // Apply reversal guard
    if (g_gate.reversal_guard_active) {
        // Apply braking until speed drops below threshold
        if (ABS(current_rpm) > GATE_REV_ALLOW_RPM) {
            speed_cmd = -GATE_REV_BRAKE_CMD * ((current_rpm > 0) ? 1 : -1);
        } else {
            speed_cmd = 0;
        }
    }
    
    // Limit to max/min speed
    if (speed_cmd > GATE_MAX_SPEED_CMD) {
        speed_cmd = GATE_MAX_SPEED_CMD;
    } else if (speed_cmd < -GATE_MAX_SPEED_CMD) {
        speed_cmd = -GATE_MAX_SPEED_CMD;
    }
    
    // Deadband
    if (ABS(speed_cmd) < GATE_MIN_SPEED_CMD) {
        speed_cmd = 0;
    }
    
    // For sliding gate, both motors move in same direction
    // Adjust signs based on your mechanical setup
    g_gate.cmd_left = speed_cmd;
    g_gate.cmd_right = speed_cmd;  // May need sign flip depending on wiring
}

static void transition_state(gate_state_t new_state) {
    if (g_gate.state != new_state) {
        g_gate.state = new_state;
    }
}

//==============================================================================
// PUBLIC API
//==============================================================================

void gate_controller_init(void) {
    memset(&g_gate, 0, sizeof(g_gate));
    g_gate.state = GATE_STATE_STOPPED;
    g_gate.armed = 0;
    g_gate.reversal_guard_active = 0;
    g_gate.cmd_left = 0;
    g_gate.cmd_right = 0;
}

void gate_controller_process(int16_t current_rpm, uint32_t current_ms) {
    // Process pending commands
    if (g_gate.pending_cmd != GATE_CMD_NONE) {
        switch (g_gate.pending_cmd) {
            case GATE_CMD_OPEN:
                if (g_gate.armed) {
                    g_gate.last_speed_cmd = g_gate.current_speed_cmd;
                    g_gate.target_speed = GATE_MAX_SPEED_CMD;
                    g_gate.ramp_start_ms = current_ms;
                    g_gate.ramp_duration_ms = GATE_RAMP_UP_MS;
                    transition_state(GATE_STATE_OPENING);
                }
                break;
                
            case GATE_CMD_CLOSE:
                if (g_gate.armed) {
                    g_gate.last_speed_cmd = g_gate.current_speed_cmd;
                    g_gate.target_speed = -GATE_MAX_SPEED_CMD;
                    g_gate.ramp_start_ms = current_ms;
                    g_gate.ramp_duration_ms = GATE_RAMP_UP_MS;
                    transition_state(GATE_STATE_CLOSING);
                }
                break;
                
            case GATE_CMD_STOP:
                g_gate.last_speed_cmd = g_gate.current_speed_cmd;
                g_gate.target_speed = 0;
                g_gate.ramp_start_ms = current_ms;
                g_gate.ramp_duration_ms = GATE_RAMP_DOWN_MS;
                transition_state(GATE_STATE_STOPPED);
                break;
                
            case GATE_CMD_CALIBRATE:
                // Calibration handled separately
                break;
                
            default:
                break;
        }
        g_gate.pending_cmd = GATE_CMD_NONE;
    }
    
    // Update ramp
    if (g_gate.target_speed != g_gate.current_speed_cmd) {
        update_ramp(current_ms, current_rpm);
    }
    
    // Check reversal guard
    check_reversal_guard(current_rpm);
    
    // Calculate final motor commands
    calculate_motor_commands(current_rpm);
    
    // State cleanup
    if (g_gate.state == GATE_STATE_OPENING || 
        g_gate.state == GATE_STATE_CLOSING) {
        if (g_gate.current_speed_cmd == 0 && ABS(current_rpm) < 5) {
            transition_state(GATE_STATE_STOPPED);
        }
    }
    
    // Error state handling
    if (g_gate.state == GATE_STATE_ERROR) {
        g_gate.cmd_left = 0;
        g_gate.cmd_right = 0;
    }
}

bool gate_controller_send_command(gate_command_t cmd) {
    if (g_gate.state == GATE_STATE_ERROR && cmd != GATE_CMD_STOP) {
        return false;
    }
    g_gate.pending_cmd = cmd;
    return true;
}

void gate_controller_set_speed(int16_t speed) {
    g_gate.last_speed_cmd = g_gate.current_speed_cmd;
    g_gate.target_speed = speed;
    g_gate.ramp_start_ms = HAL_GetTick();
    g_gate.ramp_duration_ms = (speed == 0) ? GATE_RAMP_DOWN_MS : GATE_RAMP_UP_MS;
}

gate_state_t gate_controller_get_state(void) {
    return g_gate.state;
}

bool gate_controller_is_armed(void) {
    return g_gate.armed != 0;
}

void gate_controller_set_armed(bool armed) {
    g_gate.armed = armed ? 1 : 0;
    if (!armed) {
        g_gate.target_speed = 0;
        g_gate.current_speed_cmd = 0;
        g_gate.cmd_left = 0;
        g_gate.cmd_right = 0;
        transition_state(GATE_STATE_STOPPED);
    }
}

void gate_controller_get_motor_commands(int16_t *cmd_left, int16_t *cmd_right) {
    if (cmd_left) {
        *cmd_left = g_gate.cmd_left;
    }
    if (cmd_right) {
        *cmd_right = g_gate.cmd_right;
    }
}

void gate_controller_reset(void) {
    g_gate.state = GATE_STATE_STOPPED;
    g_gate.pending_cmd = GATE_CMD_NONE;
    g_gate.target_speed = 0;
    g_gate.current_speed_cmd = 0;
    g_gate.last_speed_cmd = 0;
    g_gate.reversal_guard_active = 0;
    g_gate.cmd_left = 0;
    g_gate.cmd_right = 0;
}

bool gate_controller_is_moving(void) {
    return (g_gate.state == GATE_STATE_OPENING || 
            g_gate.state == GATE_STATE_CLOSING) &&
           (g_gate.current_speed_cmd != 0);
}

uint8_t gate_controller_get_ramp_progress(void) {
    if (g_gate.ramp_duration_ms == 0) {
        return 100;
    }
    uint32_t elapsed = HAL_GetTick() - g_gate.ramp_start_ms;
    if (elapsed >= g_gate.ramp_duration_ms) {
        return 100;
    }
    return (uint8_t)((uint32_t)elapsed * 100U / g_gate.ramp_duration_ms);
}

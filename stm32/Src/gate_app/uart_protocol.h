/**
 * @file uart_protocol.h
 * @brief GateOS UART Protocol Definition for STM32 Motor Controller
 * 
 * This module defines the binary and ASCII protocol for ESP32 <-> STM32 communication.
 * 
 * Protocol Features:
 * - Binary frames with CRC16 for motor commands (low latency)
 * - ASCII responses for telemetry and status (human readable)
 * - Keepalive mechanism for connection monitoring
 * - Command acknowledgment system
 */

#ifndef __UART_PROTOCOL_H__
#define __UART_PROTOCOL_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//==============================================================================
// PROTOCOL CONSTANTS
//==============================================================================

#define UART_BAUDRATE           115200U
#define UART_TIMEOUT_MS         50U
#define KEEPALIVE_INTERVAL_MS   100U
#define CONNECTION_TIMEOUT_MS   500U

// Frame markers
#define FRAME_START_BYTE        0xAAU
#define FRAME_END_BYTE          0x55U

// Message types (binary protocol)
typedef enum {
    MSG_TYPE_MOTOR_CMD      = 0x01U,    // Motor speed/torque command
    MSG_TYPE_ARM_REQ        = 0x02U,    // Arm/disarm request
    MSG_TYPE_MODE_REQ       = 0x03U,    // Control mode change
    MSG_TYPE_ZERO_REQ       = 0x04U,    // Zero position counter
    MSG_TYPE_CONFIG_REQ     = 0x05U,    // Configuration request
    MSG_TYPE_KEEPALIVE      = 0x06U,    // Keepalive ping
    MSG_TYPE_EMERGENCY_STOP = 0x07U,    // Emergency stop
} msg_type_t;

// Motor control modes
typedef enum {
    MOTOR_MODE_VOLTAGE  = 0x01U,
    MOTOR_MODE_SPEED    = 0x02U,
    MOTOR_MODE_TORQUE   = 0x03U,
} motor_mode_t;

// Fault codes
typedef enum {
    FAULT_NONE              = 0x0000U,
    FAULT_OVER_CURRENT      = 0x0001U,
    FAULT_OVER_TEMP         = 0x0002U,
    FAULT_UNDER_VOLTAGE     = 0x0004U,
    FAULT_ENCODER_ERROR     = 0x0008U,
    FAULT_LIMIT_SWITCH      = 0x0010U,
    FAULT_COMM_LOST         = 0x0020U,
    FAULT_SAFETY_TRIGGERED  = 0x0040U,
} fault_code_t;

//==============================================================================
// BINARY FRAME STRUCTURE
//==============================================================================

#pragma pack(push, 1)

// Motor command frame (ESP32 -> STM32)
typedef struct {
    uint8_t  start_byte;      // 0xAA
    uint8_t  msg_type;        // MSG_TYPE_MOTOR_CMD
    uint16_t cmd_left;        // Left motor command (-1000 to 1000)
    uint16_t cmd_right;       // Right motor command (-1000 to 1000)
    uint8_t  armed;           // 0 = disarm, 1 = arm
    uint8_t  mode;            // Motor mode
    uint16_t sequence;        // Sequence number for tracking
    uint16_t crc16;           // CRC16-CCITT
} motor_cmd_frame_t;

// Telemetry response frame (STM32 -> ESP32)
typedef struct {
    uint8_t  start_byte;      // 0xAA
    uint8_t  msg_type;        // MSG_TYPE_TELEMETRY
    int16_t  rpm_left;        // Left motor RPM
    int16_t  rpm_right;       // Right motor RPM
    int32_t  distance_mm;     // Cumulative distance in mm
    uint16_t fault_flags;     // Fault bitmask
    uint16_t battery_mv;      // Battery voltage in mV
    uint16_t current_ma;      // Motor current in mA
    uint8_t  armed;           // Armed status
    uint8_t  mode;            // Current mode
    uint16_t sequence;        // Echo of last command sequence
    uint16_t crc16;           // CRC16-CCITT
} telemetry_frame_t;

// Keepalive frame
typedef struct {
    uint8_t  start_byte;      // 0xAA
    uint8_t  msg_type;        // MSG_TYPE_KEEPALIVE
    uint32_t timestamp_ms;    // System uptime in ms
    uint16_t crc16;           // CRC16-CCITT
} keepalive_frame_t;

#pragma pack(pop)

//==============================================================================
// ASCII PROTOCOL COMMANDS
//==============================================================================

// Text-based commands for debugging and configuration
#define CMD_HELP          "HELP"
#define CMD_GET           "GET"
#define CMD_ZERO          "ZERO"
#define CMD_ARM           "ARM"
#define CMD_DISARM        "DISARM"
#define CMD_MODE          "MODE"
#define CMD_GETCFG        "GETCFG"
#define CMD_FW            "FW"
#define CMD_FWMAX         "FWMAX"

// Response prefixes
#define RESP_OK           "OK"
#define RESP_ERR          "ERR"
#define RESP_TEL          "TEL"

//==============================================================================
// CRC16 CALCULATION
//==============================================================================

/**
 * @brief Calculate CRC16-CCITT for a data buffer
 * @param data Pointer to data buffer
 * @param length Length of data in bytes
 * @return Calculated CRC16 value
 */
uint16_t crc16_ccitt(const uint8_t *data, uint16_t length);

/**
 * @brief Verify CRC16 of a received frame
 * @param data Pointer to frame data (including CRC bytes)
 * @param length Total length including CRC
 * @return true if CRC matches, false otherwise
 */
bool crc16_verify(const uint8_t *data, uint16_t length);

#ifdef __cplusplus
}
#endif

#endif /* __UART_PROTOCOL_H__ */

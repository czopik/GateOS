#pragma once
/**
 * @file uart_manager.h
 * @brief Event-driven UART communication manager with framing, CRC, and queues
 * 
 * Architecture:
 * - RX Task (high priority): Raw byte reception, frame assembly
 * - Parser Task: Frame validation, CRC check, command dispatch
 * - TX Task (single writer): Command queue, framed transmission
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// Frame format: [START(2)][TYPE(1)][LEN(1)][DATA(N)][CRC(2)]
#define UART_FRAME_START        0xABCD
#define UART_FRAME_MAX_DATA     64
#define UART_FRAME_MAX_SIZE     (2 + 1 + 1 + UART_FRAME_MAX_DATA + 2)
#define UART_RX_BUFFER_SIZE     512
#define UART_TX_QUEUE_SIZE      32
#define UART_RX_QUEUE_SIZE      64
#define UART_KEEPALIVE_MS       20
#define UART_TIMEOUT_MS         1000

typedef enum {
    UART_MSG_NONE = 0,
    UART_MSG_TELEMETRY,
    UART_MSG_ACK,
    UART_MSG_NACK,
    UART_MSG_CMD_ARM,
    UART_MSG_CMD_DISARM,
    UART_MSG_CMD_ZERO,
    UART_MSG_CMD_GET,
    UART_MSG_CMD_SPEED,
    UART_MSG_ERROR
} UartMsgType;

typedef struct {
    UartMsgType type;
    uint8_t data[UART_FRAME_MAX_DATA];
    uint8_t len;
    uint32_t timestamp;
    int8_t rssi;  // signal quality (optional)
} UartMessage;

typedef struct {
    int dir;
    int rpm;
    long distMm;
    int fault;
    int bat_cV;
    float batV;
    bool armed;
    int iA_x100;
    int cmdAgeMs;
    // Motor diagnostics
    int hall;
    int diag_ok;
    int diag_reason;
    int diag_edges;
    int diag_bad_state;
    int diag_bad_seq;
    int diag_dir;
} UartTelemetry;

typedef struct {
    UartTelemetry telemetry;
    uint32_t lastTelMs;
    uint32_t rxLines;
    uint32_t rxTelLines;
    uint32_t rxBadLines;
    uint32_t txCommands;
    bool connected;
    uint32_t lastActivityMs;
} UartManagerState;

class UartManager {
public:
    UartManager();
    ~UartManager();

    void begin(int rxPin, int txPin, int baud);
    void update();
    
    // Command interface (thread-safe, queue-based)
    bool sendSpeedCommand(int16_t speed);
    bool sendArmCommand();
    bool sendDisarmCommand();
    bool sendZeroCommand();
    bool sendGetCommand();
    
    // State access
    const UartManagerState& getState() const { return state; }
    const UartTelemetry& getTelemetry() const { return state.telemetry; }
    bool isTelemetryValid(uint32_t timeoutMs) const;
    bool isConnected() const { return state.connected; }
    
    // Callbacks
    using TelemetryCallback = void(*)(const UartTelemetry& tel, void* ctx);
    using StatusCallback = void(*)(bool connected, void* ctx);
    void setTelemetryCallback(TelemetryCallback cb, void* ctx);
    void setStatusCallback(StatusCallback cb, void* ctx);
    
    // Statistics
    uint32_t getRxLines() const { return state.rxLines; }
    uint32_t getRxTelLines() const { return state.rxTelLines; }
    uint32_t getRxBadLines() const { return state.rxBadLines; }
    uint32_t getTxCommands() const { return state.txCommands; }

private:
    static void rxTaskWrapper(void* pvParameters);
    static void parserTaskWrapper(void* pvParameters);
    static void txTaskWrapper(void* pvParameters);
    
    void rxTask();
    void parserTask();
    void txTask();
    
    void handleRxByte(uint8_t b);
    bool parseFrame(const uint8_t* frame, uint8_t len);
    uint16_t calcCrc(const uint8_t* data, uint8_t len);
    void sendFrame(UartMsgType type, const uint8_t* data, uint8_t len);
    void dispatchMessage(const UartMessage& msg);
    void parseTelemetry(const char* line);
    
    HardwareSerial* serial;
    int rxPin;
    int txPin;
    int baudRate;
    
    TaskHandle_t rxTaskHandle;
    TaskHandle_t parserTaskHandle;
    TaskHandle_t txTaskHandle;
    
    QueueHandle_t rxQueue;
    QueueHandle_t txQueue;
    
    UartManagerState state;
    
    // RX parser state
    uint8_t rxBuffer[UART_RX_BUFFER_SIZE];
    uint16_t rxIndex;
    uint16_t frameStart;
    bool frameBuilding;
    
    TelemetryCallback telCallback;
    StatusCallback statusCallback;
    void* callbackCtx;
    
    char lineBuf[160];
    uint16_t lineLen;
};

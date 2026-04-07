/**
 * @file uart_manager.cpp
 * @brief Event-driven UART communication manager implementation
 */

#include "uart_manager.h"
#include <esp_task_wdt.h>

#define UART_RX_TASK_STACK      4096
#define UART_PARSER_TASK_STACK  4096
#define UART_TX_TASK_STACK      3072
#define UART_RX_TASK_PRIORITY   5
#define UART_PARSER_TASK_PRIORITY 4
#define UART_TX_TASK_PRIORITY   3

static HardwareSerial* g_uartSerial = nullptr;

UartManager::UartManager() 
    : serial(nullptr)
    , rxPin(-1)
    , txPin(-1)
    , baudRate(0)
    , rxTaskHandle(nullptr)
    , parserTaskHandle(nullptr)
    , txTaskHandle(nullptr)
    , rxQueue(nullptr)
    , txQueue(nullptr)
    , rxIndex(0)
    , frameStart(0)
    , frameBuilding(false)
    , telCallback(nullptr)
    , statusCallback(nullptr)
    , callbackCtx(nullptr)
    , lineLen(0)
{
    memset(&state, 0, sizeof(state));
    memset(rxBuffer, 0, sizeof(rxBuffer));
    memset(lineBuf, 0, sizeof(lineBuf));
}

UartManager::~UartManager() {
    if (rxTaskHandle) vTaskDelete(rxTaskHandle);
    if (parserTaskHandle) vTaskDelete(parserTaskHandle);
    if (txTaskHandle) vTaskDelete(txTaskHandle);
    if (rxQueue) vQueueDelete(rxQueue);
    if (txQueue) vQueueDelete(txQueue);
}

void UartManager::begin(int rxPin_, int txPin_, int baud) {
    rxPin = rxPin_;
    txPin = txPin_;
    baudRate = baud;
    
    if (rxPin < 0 || txPin < 0 || baud <= 0) {
        Serial.println("[UART] Invalid configuration");
        return;
    }
    
    // Create queues
    rxQueue = xQueueCreate(UART_RX_QUEUE_SIZE, sizeof(uint8_t));
    txQueue = xQueueCreate(UART_TX_QUEUE_SIZE, sizeof(UartMessage));
    
    if (!rxQueue || !txQueue) {
        Serial.println("[UART] Failed to create queues");
        return;
    }
    
    // Initialize serial
    g_uartSerial = &Serial2;
    serial = g_uartSerial;
    serial->setRxBufferSize(1024);
    serial->begin(baud, SERIAL_8N1, rxPin, txPin);
    
    state.connected = true;
    state.lastActivityMs = millis();
    
    Serial.printf("[UART] Initialized RX=%d TX=%d Baud=%d\n", rxPin, txPin, baud);
    
    // Create tasks
    xTaskCreate(rxTaskWrapper, "uart_rx", UART_RX_TASK_STACK, this, 
                UART_RX_TASK_PRIORITY, &rxTaskHandle);
    xTaskCreate(parserTaskWrapper, "uart_parser", UART_PARSER_TASK_STACK, this,
                UART_PARSER_TASK_PRIORITY, &parserTaskHandle);
    xTaskCreate(txTaskWrapper, "uart_tx", UART_TX_TASK_STACK, this,
                UART_TX_TASK_PRIORITY, &txTaskHandle);
}

void UartManager::update() {
    // Check connection status
    uint32_t now = millis();
    if (state.connected && (now - state.lastActivityMs > UART_TIMEOUT_MS * 3)) {
        state.connected = false;
        if (statusCallback) {
            statusCallback(false, callbackCtx);
        }
        Serial.println("[UART] Connection lost");
    } else if (!state.connected && (now - state.lastActivityMs < UART_TIMEOUT_MS)) {
        state.connected = true;
        if (statusCallback) {
            statusCallback(true, callbackCtx);
        }
        Serial.println("[UART] Connection restored");
    }
}

bool UartManager::isTelemetryValid(uint32_t timeoutMs) const {
    if (state.lastTelMs == 0) return false;
    return (millis() - state.lastTelMs) < timeoutMs;
}

void UartManager::setTelemetryCallback(TelemetryCallback cb, void* ctx) {
    telCallback = cb;
    callbackCtx = ctx;
}

void UartManager::setStatusCallback(StatusCallback cb, void* ctx) {
    statusCallback = cb;
    callbackCtx = ctx;
}

// Command interface
bool UartManager::sendSpeedCommand(int16_t speed) {
    if (!txQueue) return false;
    
    UartMessage msg;
    msg.type = UART_MSG_CMD_SPEED;
    msg.len = 2;
    memcpy(msg.data, &speed, 2);
    msg.timestamp = millis();
    
    return xQueueSend(txQueue, &msg, pdMS_TO_TICKS(10)) == pdTRUE;
}

bool UartManager::sendArmCommand() {
    if (!txQueue) return false;
    
    UartMessage msg;
    msg.type = UART_MSG_CMD_ARM;
    msg.len = 0;
    msg.timestamp = millis();
    
    return xQueueSend(txQueue, &msg, pdMS_TO_TICKS(10)) == pdTRUE;
}

bool UartManager::sendDisarmCommand() {
    if (!txQueue) return false;
    
    UartMessage msg;
    msg.type = UART_MSG_CMD_DISARM;
    msg.len = 0;
    msg.timestamp = millis();
    
    return xQueueSend(txQueue, &msg, pdMS_TO_TICKS(10)) == pdTRUE;
}

bool UartManager::sendZeroCommand() {
    if (!txQueue) return false;
    
    UartMessage msg;
    msg.type = UART_MSG_CMD_ZERO;
    msg.len = 0;
    msg.timestamp = millis();
    
    return xQueueSend(txQueue, &msg, pdMS_TO_TICKS(10)) == pdTRUE;
}

bool UartManager::sendGetCommand() {
    if (!txQueue) return false;
    
    UartMessage msg;
    msg.type = UART_MSG_CMD_GET;
    msg.len = 0;
    msg.timestamp = millis();
    
    return xQueueSend(txQueue, &msg, pdMS_TO_TICKS(10)) == pdTRUE;
}

// Task implementations
void UartManager::rxTaskWrapper(void* pvParameters) {
    reinterpret_cast<UartManager*>(pvParameters)->rxTask();
}

void UartManager::parserTaskWrapper(void* pvParameters) {
    reinterpret_cast<UartManager*>(pvParameters)->parserTask();
}

void UartManager::txTaskWrapper(void* pvParameters) {
    reinterpret_cast<UartManager*>(pvParameters)->txTask();
}

void UartManager::rxTask() {
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(5);  // 200Hz polling
    
    while (1) {
        vTaskDelayUntil(&lastWakeTime, period);
        
        // Read available bytes
        while (serial->available()) {
            uint8_t b = serial->read();
            handleRxByte(b);
            
            // Watchdog reset for long runs
            static uint32_t counter = 0;
            if (++counter % 64 == 0) {
                esp_task_wdt_reset();
            }
        }
    }
}

void UartManager::handleRxByte(uint8_t b) {
    state.rxLines++;
    
    // Handle ASCII telemetry lines (TEL,...)
    if (b == '\r') return;
    
    if (b == '\n') {
        lineBuf[lineLen] = '\0';
        if (lineLen >= 4 && strncmp(lineBuf, "TEL,", 4) == 0) {
            parseTelemetry(lineBuf);
            state.rxTelLines++;
        } else if (lineLen > 0) {
            state.rxBadLines++;
        }
        lineLen = 0;
        return;
    }
    
    // Filter non-printable except for binary framing
    if (b < 0x20 || b > 0x7E) {
        // Could be binary frame data - handle separately if needed
        lineLen = 0;
        state.rxBadLines++;
        return;
    }
    
    if (lineLen < sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = b;
    } else {
        lineLen = 0;
        state.rxBadLines++;
    }
}

void UartManager::parseTelemetry(const char* line) {
    UartTelemetry& tel = state.telemetry;
    int dir = tel.dir;
    int rpm = tel.rpm;
    long dist = tel.distMm;
    int fault = tel.fault;
    int bat_cV = -1;
    bool haveBatCV = false;
    int iA_val = -1;
    bool have_iA = false;
    int armedVal = -1;
    int cmd_age_ms = -1;
    bool have_cmd_age = false;
    
    int hall = tel.hall;
    int diag_ok = tel.diag_ok;
    int diag_reason = tel.diag_reason;
    int diag_edges = tel.diag_edges;
    int diag_bad_state = tel.diag_bad_state;
    int diag_bad_seq = tel.diag_bad_seq;
    int diag_dir = tel.diag_dir;
    
    char tmp[160];
    strncpy(tmp, line + 4, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    
    char* saveptr = nullptr;
    char* tok = strtok_r(tmp, ",", &saveptr);
    while (tok) {
        if (strncmp(tok, "dir=", 4) == 0) dir = atoi(tok + 4);
        else if (strncmp(tok, "rpm=", 4) == 0) rpm = atoi(tok + 4);
        else if (strncmp(tok, "dist_mm=", 8) == 0) dist = atol(tok + 8);
        else if (strncmp(tok, "fault=", 6) == 0) fault = atoi(tok + 6);
        else if (strncmp(tok, "bat_cV=", 7) == 0) { bat_cV = atoi(tok + 7); haveBatCV = true; }
        else if (strncmp(tok, "iA=", 3) == 0) { iA_val = atoi(tok + 3); have_iA = true; }
        else if (strncmp(tok, "armed=", 6) == 0) { armedVal = atoi(tok + 6); }
        else if (strncmp(tok, "cmd_age_ms=", 11) == 0) { cmd_age_ms = atoi(tok + 11); have_cmd_age = true; }
        else if (strncmp(tok, "hall=", 5) == 0) { hall = atoi(tok + 5); }
        else if (strncmp(tok, "diag_ok=", 8) == 0) { diag_ok = atoi(tok + 8); }
        else if (strncmp(tok, "diag_reason=", 12) == 0) { diag_reason = atoi(tok + 12); }
        else if (strncmp(tok, "diag_edges=", 11) == 0) { diag_edges = atoi(tok + 11); }
        else if (strncmp(tok, "diag_bad_state=", 15) == 0) { diag_bad_state = atoi(tok + 15); }
        else if (strncmp(tok, "diag_bad_seq=", 13) == 0) { diag_bad_seq = atoi(tok + 13); }
        else if (strncmp(tok, "diag_dir=", 9) == 0) { diag_dir = atoi(tok + 9); }
        tok = strtok_r(nullptr, ",", &saveptr);
    }
    
    tel.dir = dir;
    tel.rpm = rpm;
    tel.distMm = dist;
    tel.fault = fault;
    
    if (haveBatCV) {
        tel.bat_cV = bat_cV;
        tel.batV = (float)bat_cV / 100.0f;
    }
    if (have_iA) tel.iA_x100 = iA_val;
    if (armedVal >= 0) tel.armed = (armedVal != 0);
    tel.cmdAgeMs = have_cmd_age ? cmd_age_ms : -1;
    
    tel.hall = hall;
    tel.diag_ok = diag_ok;
    tel.diag_reason = diag_reason;
    tel.diag_edges = diag_edges;
    tel.diag_bad_state = diag_bad_state;
    tel.diag_bad_seq = diag_bad_seq;
    tel.diag_dir = diag_dir;
    
    state.lastTelMs = millis();
    state.lastActivityMs = millis();
    
    // Callback
    if (telCallback) {
        telCallback(tel, callbackCtx);
    }
}

void UartManager::parserTask() {
    // Currently ASCII-only parsing in RX task
    // Binary frame parser would go here if needed
    vTaskSuspend(nullptr);  // Not used for now
}

void UartManager::txTask() {
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(UART_KEEPALIVE_MS);
    
    uint32_t lastKeepaliveMs = 0;
    
    while (1) {
        vTaskDelayUntil(&lastWakeTime, period);
        uint32_t now = millis();
        
        UartMessage msg;
        
        // Process queued commands
        while (xQueueReceive(txQueue, &msg, 0) == pdTRUE) {
            switch (msg.type) {
                case UART_MSG_CMD_SPEED: {
                    int16_t speed;
                    memcpy(&speed, msg.data, 2);
                    // Send binary frame or ASCII command
                    char cmd[32];
                    snprintf(cmd, sizeof(cmd), "SPEED,%d\n", speed);
                    serial->print(cmd);
                    break;
                }
                case UART_MSG_CMD_ARM:
                    serial->print("ARM\n");
                    break;
                case UART_MSG_CMD_DISARM:
                    serial->print("DISARM\n");
                    break;
                case UART_MSG_CMD_ZERO:
                    serial->print("ZERO\n");
                    break;
                case UART_MSG_CMD_GET:
                    serial->print("GET\n");
                    break;
                default:
                    break;
            }
            state.txCommands++;
            state.lastActivityMs = now;
        }
        
        // Keepalive: request telemetry periodically
        if (now - lastKeepaliveMs >= 50) {
            serial->print("GET\n");
            lastKeepaliveMs = now;
        }
    }
}

uint16_t UartManager::calcCrc(const uint8_t* data, uint8_t len) {
    uint16_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

void UartManager::sendFrame(UartMsgType type, const uint8_t* data, uint8_t len) {
    // Binary frame sending (future use)
}

bool UartManager::parseFrame(const uint8_t* frame, uint8_t len) {
    // Binary frame parsing (future use)
    return false;
}

void UartManager::dispatchMessage(const UartMessage& msg) {
    // Message dispatch (future use for binary protocol)
}

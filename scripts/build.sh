#!/bin/bash
# GateOS Build and Upload Script
# Usage: ./build.sh [esp32|stm32|all] [upload]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

ESP32_DIR="$PROJECT_ROOT/esp32"
STM32_DIR="$PROJECT_ROOT/stm32"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

build_esp32() {
    log_info "Building ESP32 firmware..."
    
    cd "$ESP32_DIR"
    
    if command -v pio &> /dev/null; then
        log_info "Using PlatformIO CLI"
        pio run
    elif command -v platformio &> /dev/null; then
        log_info "Using platformio"
        platformio run
    else
        log_error "PlatformIO not found. Install with: pip install platformio"
        exit 1
    fi
    
    log_info "ESP32 build complete"
}

upload_esp32() {
    log_info "Uploading ESP32 firmware..."
    
    cd "$ESP32_DIR"
    
    if command -v pio &> /dev/null; then
        pio run --target upload
    elif command -v platformio &> /dev/null; then
        platformio run --target upload
    else
        log_error "PlatformIO not found"
        exit 1
    fi
    
    log_info "ESP32 upload complete"
}

upload_fs_esp32() {
    log_info "Uploading ESP32 filesystem (LittleFS)..."
    
    cd "$ESP32_DIR"
    
    if command -v pio &> /dev/null; then
        pio run --target uploadfs
    elif command -v platformio &> /dev/null; then
        platformio run --target uploadfs
    else
        log_error "PlatformIO not found"
        exit 1
    fi
    
    log_info "Filesystem upload complete"
}

build_stm32() {
    log_info "Building STM32 firmware..."
    
    cd "$STM32_DIR"
    
    # Check for Makefile or IDE project
    if [ -f "Makefile" ]; then
        make clean
        make
    elif [ -d ".settings" ] && command -v stm32cubeide-cli &> /dev/null; then
        log_info "Using STM32CubeIDE CLI"
        stm32cubeide-cli -application com.st.stm32cube.ide.mcu.ide.build \
            -projectLocation "$STM32_DIR" -cleanBuild
    elif [ -f "*.ioc" ] && command -v STM32CubeMX &> /dev/null; then
        log_warn "STM32CubeMX project detected. Please build in IDE."
    else
        log_warn "No build system detected for STM32"
        log_info "Expected: Makefile, STM32CubeIDE project, or Keil project"
    fi
    
    log_info "STM32 build complete (or skipped)"
}

upload_stm32() {
    log_info "Uploading STM32 firmware..."
    
    cd "$STM32_DIR"
    
    # Try common upload methods
    if [ -f "build/*.bin" ] || [ -f "*.bin" ]; then
        BIN_FILE=$(find . -name "*.bin" -type f | head -1)
        
        if command -v stm32flash &> /dev/null; then
            log_info "Using stm32flash"
            stm32flash -w "$BIN_FILE" /dev/ttyUSB0
        elif command -v dfu-util &> /dev/null; then
            log_info "Using dfu-util"
            dfu-util -D "$BIN_FILE"
        elif command -v openocd &> /dev/null; then
            log_info "Using OpenOCD"
            openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
                -c "program $BIN_FILE verify reset exit"
        else
            log_error "No STM32 programmer found (stm32flash, dfu-util, or openocd)"
            exit 1
        fi
        
        log_info "STM32 upload complete"
    else
        log_error "No STM32 binary found. Build first."
        exit 1
    fi
}

run_tests() {
    log_info "Running automated tests..."
    
    cd "$PROJECT_ROOT/tests"
    
    if [ ! -f "test_gate.py" ]; then
        log_error "Test script not found"
        exit 1
    fi
    
    python3 test_gate.py --url "${TEST_GATE_URL:-http://gate.local}" "$@"
}

format_code() {
    log_info "Formatting code..."
    
    # Format C/C++ files
    if command -v clang-format &> /dev/null; then
        find "$PROJECT_ROOT/esp32/Src" -name "*.cpp" -o -name "*.h" | \
            xargs clang-format -i
        find "$PROJECT_ROOT/stm32/Src" -name "*.c" -o -name "*.h" | \
            xargs clang-format -i
        log_info "Code formatted with clang-format"
    else
        log_warn "clang-format not found"
    fi
}

check_dependencies() {
    log_info "Checking dependencies..."
    
    MISSING=()
    
    if ! command -v python3 &> /dev/null; then
        MISSING+=("python3")
    fi
    
    if ! command -v git &> /dev/null; then
        MISSING+=("git")
    fi
    
    # Check Python packages
    if ! python3 -c "import requests" 2>/dev/null; then
        MISSING+=("python3-requests (pip install requests)")
    fi
    
    if ! python3 -c "import websocket" 2>/dev/null; then
        MISSING+=("python3-websocket-client (pip install websocket-client)")
    fi
    
    if [ ${#MISSING[@]} -ne 0 ]; then
        log_warn "Missing dependencies:"
        for dep in "${MISSING[@]}"; do
            echo "  - $dep"
        done
        echo ""
        log_info "Install with: pip install requests websocket-client"
    else
        log_info "All dependencies satisfied"
    fi
}

show_help() {
    cat << EOF
GateOS Build System

Usage: $0 [command] [options]

Commands:
    build           Build all firmware (ESP32 + STM32)
    build-esp32     Build ESP32 firmware only
    build-stm32     Build STM32 firmware only
    upload          Upload all firmware
    upload-esp32    Upload ESP32 firmware
    upload-stm32    Upload STM32 firmware
    upload-fs       Upload ESP32 filesystem
    test            Run automated tests
    format          Format source code
    deps            Check dependencies
    all             Build + upload all
    help            Show this help

Options:
    --port PORT     Serial port for upload (default: auto-detect)
    --verbose       Enable verbose output

Examples:
    $0 build
    $0 upload-esp32 --port /dev/ttyUSB0
    $0 test --url http://192.168.1.100
    $0 all

EOF
}

# Main
case "${1:-help}" in
    build)
        check_dependencies
        build_esp32
        build_stm32
        ;;
    build-esp32)
        build_esp32
        ;;
    build-stm32)
        build_stm32
        ;;
    upload)
        upload_esp32
        upload_stm32
        ;;
    upload-esp32)
        upload_esp32
        ;;
    upload-stm32)
        upload_stm32
        ;;
    upload-fs)
        upload_fs_esp32
        ;;
    test)
        shift
        run_tests "$@"
        ;;
    format)
        format_code
        ;;
    deps)
        check_dependencies
        ;;
    all)
        check_dependencies
        build_esp32
        build_stm32
        upload_esp32
        upload_stm32
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        log_error "Unknown command: $1"
        show_help
        exit 1
        ;;
esac

exit 0

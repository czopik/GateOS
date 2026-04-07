#!/bin/bash
# GateOS Build Script for STM32
# This script builds the STM32 hoverboard firmware with GateOS extensions

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
STM32_DIR="$PROJECT_DIR/stm32"
BUILD_DIR="$STM32_DIR/build"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}  GateOS STM32 Build Script${NC}"
echo -e "${GREEN}======================================${NC}"

# Check if toolchain is available
check_toolchain() {
    if command -v arm-none-eabi-gcc &> /dev/null; then
        echo -e "${GREEN}✓ ARM GCC toolchain found${NC}"
        TOOLCHAIN_PREFIX="arm-none-eabi-"
    elif [ -n "$TOOLCHAIN_PATH" ]; then
        if [ -x "$TOOLCHAIN_PATH/arm-none-eabi-gcc" ]; then
            echo -e "${GREEN}✓ ARM GCC toolchain found at $TOOLCHAIN_PATH${NC}"
            TOOLCHAIN_PREFIX="$TOOLCHAIN_PATH/arm-none-eabi-"
        else
            echo -e "${RED}✗ ARM GCC not found at TOOLCHAIN_PATH${NC}"
            return 1
        fi
    else
        echo -e "${YELLOW}⚠ ARM GCC toolchain not found in PATH${NC}"
        echo "Please install: sudo apt install gcc-arm-none-eabi"
        echo "Or set TOOLCHAIN_PATH environment variable"
        return 1
    fi
}

# Create build directory
setup_build_dir() {
    mkdir -p "$BUILD_DIR"
    echo -e "${GREEN}✓ Build directory created: $BUILD_DIR${NC}"
}

# Compile a single C file
compile_file() {
    local src_file="$1"
    local obj_file="$2"
    local includes="$3"
    
    ${TOOLCHAIN_PREFIX}gcc \
        -mcpu=cortex-m3 \
        -mthumb \
        -mlittle-endian \
        -mfpu=vfp \
        -mfloat-abi=soft \
        -O2 \
        -fdata-sections \
        -ffunction-sections \
        -g \
        -gdwarf-2 \
        -Wall \
        -Wextra \
        -DSTM32F10X_MD \
        -DUSE_STDPERIPH_DRIVER \
        -DVARIANT_USART \
        -DFEEDBACK_SERIAL_USART2 \
        $includes \
        -I"$STM32_DIR/Src" \
        -I"$STM32_DIR/Src/gate_app" \
        -c "$src_file" \
        -o "$obj_file"
}

# Main build function
build() {
    echo ""
    echo -e "${YELLOW}Building GateOS STM32 firmware...${NC}"
    echo ""
    
    check_toolchain || exit 1
    setup_build_dir
    
    local SRC_DIR="$STM32_DIR/Src"
    local GATE_APP_DIR="$SRC_DIR/gate_app"
    local INCLUDES="-I$SRC_DIR -I$GATE_APP_DIR"
    
    # Source files to compile (GateOS additions)
    local gate_files=(
        "$GATE_APP_DIR/uart_protocol.c"
        "$GATE_APP_DIR/gate_controller.c"
    )
    
    # Compile GateOS modules
    echo "Compiling GateOS modules..."
    for src in "${gate_files[@]}"; do
        local basename=$(basename "$src" .c)
        local obj="$BUILD_DIR/${basename}.o"
        echo "  $basename.c -> ${basename}.o"
        compile_file "$src" "$obj" "$INCLUDES"
    done
    
    # Link step (placeholder - full build requires complete project setup)
    echo ""
    echo -e "${YELLOW}Note: Full linking requires complete STM32 project setup.${NC}"
    echo "The following object files were created:"
    ls -la "$BUILD_DIR"/*.o 2>/dev/null || echo "  (none yet)"
    
    echo ""
    echo -e "${GREEN}✓ GateOS modules compiled successfully${NC}"
    echo ""
    echo "To complete the build:"
    echo "  1. Import all source files into STM32CubeIDE"
    echo "  2. Add gate_app/ to include paths"
    echo "  3. Build the complete project"
    echo ""
}

# Flash firmware using st-flash
flash() {
    echo ""
    echo -e "${YELLOW}Flashing firmware...${NC}"
    
    if [ ! -f "$1" ]; then
        echo -e "${RED}Error: Binary file not found: $1${NC}"
        exit 1
    fi
    
    if ! command -v st-flash &> /dev/null; then
        echo -e "${RED}Error: st-flash not found. Install stlink-tools.${NC}"
        exit 1
    fi
    
    echo "Flashing $1..."
    st-flash write "$1" 0x8000000
    
    echo -e "${GREEN}✓ Flash complete${NC}"
}

# Clean build artifacts
clean() {
    echo ""
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf "$BUILD_DIR"
    echo -e "${GREEN}✓ Clean complete${NC}"
}

# Show help
show_help() {
    echo ""
    echo "Usage: $0 <command>"
    echo ""
    echo "Commands:"
    echo "  build     Build GateOS STM32 modules"
    echo "  flash     Flash binary to STM32 (requires .bin file path)"
    echo "  clean     Remove build artifacts"
    echo "  help      Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 build"
    echo "  $0 flash build/GateOS.bin"
    echo "  $0 clean"
    echo ""
}

# Main entry point
case "${1:-build}" in
    build)
        build
        ;;
    flash)
        flash "$2"
        ;;
    clean)
        clean
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        echo -e "${RED}Unknown command: $1${NC}"
        show_help
        exit 1
        ;;
esac

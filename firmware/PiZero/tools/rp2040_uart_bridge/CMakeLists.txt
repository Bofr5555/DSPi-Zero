# RP2040 USB-UART bridge for reading the Pi Zero's log. Standalone build against the repo's pico-sdk.
#   cmake -S firmware/PiZero/tools/rp2040_uart_bridge -B firmware/PiZero/build/uart_bridge -G Ninja
cmake_minimum_required(VERSION 3.13)

set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../../DSPi/firmware/pico-sdk)
set(PICO_BOARD pico)
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(rp2040_uart_bridge C CXX ASM)
pico_sdk_init()

add_executable(rp2040_uart_bridge main.c)
target_link_libraries(rp2040_uart_bridge pico_stdlib)
pico_enable_stdio_usb(rp2040_uart_bridge 1)
pico_enable_stdio_uart(rp2040_uart_bridge 0)
pico_set_program_name(rp2040_uart_bridge "DSPi PiZero UART bridge")
pico_add_extra_outputs(rp2040_uart_bridge)

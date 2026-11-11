# Bare-metal DSPi platform layer for the Raspberry Pi Zero (BCM2835) and Zero 2 W (BCM2837, 32-bit).
# Standalone build, not part of firmware/CMakeLists.txt. One build directory per board:
#   cmake -S firmware/PiZero -B firmware/PiZero/build -G Ninja && cmake --build firmware/PiZero/build
#   cmake -S firmware/PiZero -B firmware/PiZero/build-zero2 -G Ninja -DPIZERO_BOARD=zero2 && cmake --build ...
cmake_minimum_required(VERSION 3.13)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/cmake/arm-none-eabi.cmake)
endif()

project(dspi_pizero C ASM)

# Optimisation is set per target below, so drop CMake's per-build-type -O flags.
set(CMAKE_C_FLAGS_RELEASE "")
set(CMAKE_ASM_FLAGS_RELEASE "")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Adafruit broadcom-peripherals, fetched by: python3 tools/get_deps.py broadcom_32bit (inside TinyUSB).
set(BCM_DIR ${CMAKE_CURRENT_LIST_DIR}/../pico-sdk/lib/tinyusb/hw/mcu/broadcom)
if(NOT EXISTS ${BCM_DIR}/broadcom/vcmailbox.c)
    message(FATAL_ERROR "broadcom-peripherals missing. Run: cd firmware/pico-sdk/lib/tinyusb && python3 tools/get_deps.py broadcom_32bit")
endif()

set(PIZERO_BOARD zero CACHE STRING "Target board: zero (BCM2835) or zero2 (BCM2837, AArch32)")
set_property(CACHE PIZERO_BOARD PROPERTY STRINGS zero zero2)
if(PIZERO_BOARD STREQUAL "zero")
    set(CPU_FLAGS -mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard -marm)
    set(BCM_VERSION 2835)
elseif(PIZERO_BOARD STREQUAL "zero2")
    set(CPU_FLAGS -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard -marm)
    set(BCM_VERSION 2837)
else()
    message(FATAL_ERROR "PIZERO_BOARD must be zero or zero2, not ${PIZERO_BOARD}")
endif()
message(STATUS "PiZero board: ${PIZERO_BOARD} (BCM${BCM_VERSION})")

# With the MMU off every data access is strongly ordered, where unaligned accesses fault.
set(COMMON_FLAGS ${CPU_FLAGS} -ffreestanding -fno-common -ffunction-sections -fdata-sections -mno-unaligned-access)

# Built at -O0, the way TinyUSB's broadcom_32bit BSP builds and tests it. We use the GPIO helpers,
# the cache helpers TinyUSB's DWC2 glue links against, and the register headers. mbox.c replaces vcmailbox.c.
if(BCM_VERSION EQUAL 2835)
    set(BCM_CACHES ${BCM_DIR}/broadcom/caches.c)
else()
    set(BCM_CACHES ${CMAKE_CURRENT_LIST_DIR}/caches_v7.c)
endif()
add_library(bcm_peripherals STATIC
    ${BCM_DIR}/broadcom/gpio.c
    ${BCM_CACHES}
)
target_include_directories(bcm_peripherals PUBLIC ${BCM_DIR})
target_compile_definitions(bcm_peripherals PUBLIC BCM_VERSION=${BCM_VERSION})
target_compile_options(bcm_peripherals PRIVATE ${COMMON_FLAGS} -O0 -std=c17 -w)

set(PLATFORM_SRC startup.S board.c uart.c mbox.c bootcheck.c)

# Each milestone is its own image. <target>.img is the raw image the GPU firmware loads at 0x8000.
# The latest milestone (SDCARD_TARGET) is also staged as sdcard/kernel.img next to config.txt.
set(SDCARD_TARGET pizero_z2)
# An optional second argument names another linker script (the loader's); both INCLUDE pizero_sections.ld.
function(pizero_image target)
    set(ldscript pizero.ld)
    if(ARGC GREATER 1)
        set(ldscript ${ARGV1})
    endif()
    target_compile_options(${target} PRIVATE ${COMMON_FLAGS} -O2 -g -std=c17 -Wall -Wextra)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/loader)
    target_link_libraries(${target} PRIVATE bcm_peripherals)
    target_link_options(${target} PRIVATE
        ${CPU_FLAGS}
        -nostartfiles
        --specs=nano.specs
        --specs=nosys.specs
        -L${CMAKE_CURRENT_LIST_DIR}
        -T${CMAKE_CURRENT_LIST_DIR}/${ldscript}
        -Wl,--gc-sections
        -Wl,--no-warn-rwx-segments
        -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${target}.map
    )
    set_target_properties(${target} PROPERTIES SUFFIX ".elf"
        LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/${ldscript};${CMAKE_CURRENT_LIST_DIR}/pizero_sections.ld")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${target}> ${CMAKE_CURRENT_BINARY_DIR}/${target}.img
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${target}>
        VERBATIM
    )
    if(target STREQUAL SDCARD_TARGET)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/sdcard
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/${target}.img ${CMAKE_CURRENT_BINARY_DIR}/sdcard/kernel.img
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/boot/config.txt ${CMAKE_CURRENT_BINARY_DIR}/sdcard/config.txt
            VERBATIM
        )
    endif()
endfunction()

# Z0: LED blink and UART heartbeat.
add_executable(pizero_z0 ${PLATFORM_SRC} main_z0.c)
pizero_image(pizero_z0)

# Z1: TinyUSB vendor echo on the DWC2 controller, polled from the main loop.
set(TUSB_DIR ${CMAKE_CURRENT_LIST_DIR}/../pico-sdk/lib/tinyusb/src)
add_library(tinyusb_z1 STATIC
    ${TUSB_DIR}/tusb.c
    ${TUSB_DIR}/common/tusb_fifo.c
    ${TUSB_DIR}/device/usbd.c
    ${TUSB_DIR}/device/usbd_control.c
    ${TUSB_DIR}/class/vendor/vendor_device.c
    ${TUSB_DIR}/portable/synopsys/dwc2/dcd_dwc2.c
    ${TUSB_DIR}/portable/synopsys/dwc2/dwc2_common.c
)
target_include_directories(tinyusb_z1 PUBLIC ${TUSB_DIR} ${CMAKE_CURRENT_LIST_DIR}/z1)
target_link_libraries(tinyusb_z1 PUBLIC bcm_peripherals)
target_compile_options(tinyusb_z1 PRIVATE ${COMMON_FLAGS} -O2 -g -std=c17 -Wall)

add_executable(pizero_z1 ${PLATFORM_SRC} z1/main_z1.c z1/usb_descriptors.c)
target_link_libraries(pizero_z1 PRIVATE tinyusb_z1)
pizero_image(pizero_z1)

# Z2: DSPi's own UAC1 descriptors (firmware/DSPi/usb_descriptors.c, stereo alts only, product string renamed)
# with a cut-down copy of its class driver. z2/shim stands in for the one Pico SDK header it includes.
set(DSPI_DIR ${CMAKE_CURRENT_LIST_DIR}/../DSPi)
add_library(tinyusb_z2 STATIC
    ${TUSB_DIR}/tusb.c
    ${TUSB_DIR}/common/tusb_fifo.c
    ${TUSB_DIR}/device/usbd.c
    ${TUSB_DIR}/device/usbd_control.c
    ${TUSB_DIR}/portable/synopsys/dwc2/dcd_dwc2.c
    ${TUSB_DIR}/portable/synopsys/dwc2/dwc2_common.c
)
target_include_directories(tinyusb_z2 PUBLIC ${TUSB_DIR} ${CMAKE_CURRENT_LIST_DIR}/z2)
target_link_libraries(tinyusb_z2 PUBLIC bcm_peripherals)
target_compile_options(tinyusb_z2 PRIVATE ${COMMON_FLAGS} -O2 -g -std=c17 -Wall)

# The product string becomes "Weeb Labs DSPi Zero". The copy is regenerated whenever the DSPi file changes,
# and configuring fails if the string it replaces is no longer there.
set(DSPI_PRODUCT_FROM "\"Weeb Labs DSPi\",")
set(DSPI_PRODUCT_TO "\"Weeb Labs DSPi Zero\",")
file(READ ${DSPI_DIR}/usb_descriptors.c DSPI_DESC_SRC)
string(FIND "${DSPI_DESC_SRC}" "${DSPI_PRODUCT_FROM}" DSPI_PRODUCT_POS)
if(DSPI_PRODUCT_POS EQUAL -1)
    message(FATAL_ERROR "DSPi product string not found in ${DSPI_DIR}/usb_descriptors.c")
endif()
string(REPLACE "${DSPI_PRODUCT_FROM}" "${DSPI_PRODUCT_TO}" DSPI_DESC_SRC "${DSPI_DESC_SRC}")
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/z2/usb_descriptors_zero.c CONTENT "${DSPI_DESC_SRC}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DSPI_DIR}/usb_descriptors.c)

add_library(dspi_descriptors_z2 STATIC ${CMAKE_CURRENT_BINARY_DIR}/z2/usb_descriptors_zero.c)
target_include_directories(dspi_descriptors_z2 BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/z2 ${CMAKE_CURRENT_LIST_DIR}/z2/shim)
target_include_directories(dspi_descriptors_z2 PRIVATE ${DSPI_DIR})
target_link_libraries(dspi_descriptors_z2 PUBLIC tinyusb_z2)
target_compile_options(dspi_descriptors_z2 PRIVATE ${COMMON_FLAGS} -O2 -g -std=c17 -Wall)

add_executable(pizero_z2 ${PLATFORM_SRC} z2/main_z2.c z2/uac1_z2.c)
# z2 first, so its tusb_config.h wins over DSPi's Pico one.
target_include_directories(pizero_z2 PRIVATE ${CMAKE_CURRENT_LIST_DIR}/z2 ${DSPI_DIR})
target_link_libraries(pizero_z2 PRIVATE dspi_descriptors_z2 tinyusb_z2)
pizero_image(pizero_z2)

# USB loader: the SD card's kernel.img once it is set up (sdcard-loader/), loaded at 32 MB. It receives
# any image above over USB into 0x8000 and starts it (tools/pizero_load.py).
add_library(tinyusb_loader STATIC
    ${TUSB_DIR}/tusb.c
    ${TUSB_DIR}/common/tusb_fifo.c
    ${TUSB_DIR}/device/usbd.c
    ${TUSB_DIR}/device/usbd_control.c
    ${TUSB_DIR}/class/vendor/vendor_device.c
    ${TUSB_DIR}/portable/synopsys/dwc2/dcd_dwc2.c
    ${TUSB_DIR}/portable/synopsys/dwc2/dwc2_common.c
)
target_include_directories(tinyusb_loader PUBLIC ${TUSB_DIR} ${CMAKE_CURRENT_LIST_DIR}/loader)
target_link_libraries(tinyusb_loader PUBLIC bcm_peripherals)
target_compile_options(tinyusb_loader PRIVATE ${COMMON_FLAGS} -O2 -g -std=c17 -Wall)

add_executable(pizero_loader ${PLATFORM_SRC} loader/main_loader.c loader/usb_descriptors.c)
target_link_libraries(pizero_loader PRIVATE tinyusb_loader)
pizero_image(pizero_loader pizero_loader.ld)
add_custom_command(TARGET pizero_loader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/sdcard-loader
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/pizero_loader.img ${CMAKE_CURRENT_BINARY_DIR}/sdcard-loader/kernel.img
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/boot/config_loader.txt ${CMAKE_CURRENT_BINARY_DIR}/sdcard-loader/config.txt
    VERBATIM
)

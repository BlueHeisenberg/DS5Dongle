# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if (WIN32)
    set(USERHOME $ENV{USERPROFILE})
else ()
    set(USERHOME $ENV{HOME})
endif ()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif ()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

#set(PICO_CYW43_SUPPORTED 1)
#set(PICO_CYW43_ARCH_THREADSAFE_BACKGROUND 1)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(ds5-bridge C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# PIO-USB software host (donor Xbox controller port) — provides `pico_pio_usb`.
add_subdirectory(lib/Pico-PIO-USB)
if(NOT PICO_TINYUSB_PATH)
    set(PICO_TINYUSB_PATH ${PICO_SDK_PATH}/lib/tinyusb)
endif()

# Add executable. Default name is the project name, version 0.1

add_executable(ds5-bridge
        src/usb_descriptors.c
        src/main.cpp
        src/usb.cpp
        src/audio.cpp
        src/bt.cpp
)

# Cockos WDLResampler
add_library(wdl_resampler STATIC
        lib/WDL/WDL/resample.cpp
)
target_include_directories(wdl_resampler PUBLIC
        lib/WDL/WDL
)

# Opus
#set(OPUS_FIXED_POINT ON)
add_subdirectory(lib/opus)

target_compile_options(wdl_resampler PRIVATE -fsigned-char)
target_compile_options(ds5-bridge PRIVATE -fsigned-char)
target_compile_definitions(wdl_resampler PRIVATE
        WDL_RESAMPLE_TYPE=float
)
target_compile_definitions(ds5-bridge PRIVATE
        CYW43_LWIP=0
        PICO_FLASH_ASSUME_CORE1_SAFE=1
        WDL_RESAMPLE_TYPE=float
        CYW43_PIO_CLOCK_DIV_INT=4
)
option(ENABLE_DSE "Enable DSE" OFF)
if(ENABLE_DSE)
    target_compile_definitions(ds5-bridge PRIVATE
            ENABLE_DSE # 将PID和产品名称设置为DSE
    )
endif()

pico_set_program_name(ds5-bridge "ds5-bridge")
pico_set_program_version(ds5-bridge "0.5.3")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(ds5-bridge 1)
pico_enable_stdio_usb(ds5-bridge 0)

# Add the standard include files to the build
#target_include_directories(ds5-bridge PRIVATE
#        ${CMAKE_CURRENT_LIST_DIR}
#)

target_include_directories(${PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add any user requested libraries
target_link_libraries(ds5-bridge
        pico_stdlib
        pico_multicore
        hardware_watchdog
        hardware_timer
        pico_btstack_classic
#        pico_cyw43_arch_threadsafe_background
        pico_cyw43_arch_poll
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
        wdl_resampler
        opus
)

pico_add_extra_outputs(ds5-bridge)

# ---- Hardware bring-up smoke test (LED + I2C OLED) ----
# Minimal target: does not pull in BT / USB / Opus. Build with:  ninja bringup_test
add_executable(bringup_test
        src/bringup/bringup_test.cpp
        src/bringup/ssd1306.cpp
)
target_include_directories(bringup_test PRIVATE src/bringup)
pico_set_program_name(bringup_test "bringup_test")
pico_enable_stdio_uart(bringup_test 1)
pico_enable_stdio_usb(bringup_test 0)
target_link_libraries(bringup_test
        pico_stdlib
        pico_cyw43_arch_none
        hardware_i2c
)
pico_add_extra_outputs(bringup_test)

# ---- PIO-USB host bring-up test (enumerate a USB device, show VID/PID) ----
# Build with:  ninja usbhost_test
add_executable(usbhost_test
        src/usbhost/usbhost_test.cpp
        src/bringup/ssd1306.cpp
        ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)
target_include_directories(usbhost_test PRIVATE src/usbhost src/bringup)
pico_set_program_name(usbhost_test "usbhost_test")
pico_enable_stdio_uart(usbhost_test 1)
pico_enable_stdio_usb(usbhost_test 0)   # SDK forbids stdio-USB alongside a TinyUSB host
target_compile_definitions(usbhost_test PRIVATE CFG_TUH_RPI_PIO_USB=1)
target_link_libraries(usbhost_test
        pico_stdlib
        pico_multicore
        tinyusb_host
        pico_pio_usb
        hardware_i2c
        hardware_dma
)
pico_add_extra_outputs(usbhost_test)

# ---- GIP descriptor probe (dump a device's descriptors; clone the controller) ----
add_executable(gip_probe
        src/gipprobe/gip_probe.cpp
        src/bringup/ssd1306.cpp
        ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)
target_include_directories(gip_probe PRIVATE src/gipprobe src/usbhost src/bringup)
pico_set_program_name(gip_probe "gip_probe")
pico_enable_stdio_uart(gip_probe 1)
pico_enable_stdio_usb(gip_probe 0)
target_compile_definitions(gip_probe PRIVATE CFG_TUH_RPI_PIO_USB=1)
target_link_libraries(gip_probe
        pico_stdlib
        pico_multicore
        tinyusb_host
        pico_pio_usb
        hardware_i2c
        hardware_dma
)
pico_add_extra_outputs(gip_probe)

# ---- GIP capture: drive the controller as GIP host, record its packets to flash ----
add_executable(gip_capture
        src/gipprobe/gip_capture.cpp
        src/bringup/ssd1306.cpp
        ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)
target_include_directories(gip_capture PRIVATE src/gipprobe src/usbhost src/bringup)
pico_set_program_name(gip_capture "gip_capture")
pico_enable_stdio_uart(gip_capture 1)
pico_enable_stdio_usb(gip_capture 0)
target_compile_definitions(gip_capture PRIVATE CFG_TUH_RPI_PIO_USB=1)
target_link_libraries(gip_capture
        pico_stdlib
        pico_multicore
        tinyusb_host
        pico_pio_usb
        hardware_i2c
        hardware_dma
        hardware_flash
)
pico_add_extra_outputs(gip_capture)

# ---- Class-driver test (MSC pendrive + HID) to validate PIO-USB IN transfers ----
add_executable(class_test
        src/classtest/class_test.cpp
        src/bringup/ssd1306.cpp
        ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)
target_include_directories(class_test PRIVATE src/classtest src/bringup)
pico_set_program_name(class_test "class_test")
pico_enable_stdio_uart(class_test 1)
pico_enable_stdio_usb(class_test 0)
target_compile_definitions(class_test PRIVATE CFG_TUH_RPI_PIO_USB=1)
target_link_libraries(class_test
        pico_stdlib
        pico_multicore
        tinyusb_host
        pico_pio_usb
        hardware_i2c
        hardware_dma
)
pico_add_extra_outputs(class_test)

# ---- GIP host class driver (finally read the controller properly) ----
add_executable(gip_hostdrv
        src/gipdrv/gip_main.cpp
        src/gipdrv/gip_host.c
        src/bringup/ssd1306.cpp
        ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)
target_include_directories(gip_hostdrv PRIVATE src/gipdrv src/bringup ${PICO_TINYUSB_PATH}/src)
pico_set_program_name(gip_hostdrv "gip_hostdrv")
pico_enable_stdio_uart(gip_hostdrv 1)
pico_enable_stdio_usb(gip_hostdrv 0)
target_compile_definitions(gip_hostdrv PRIVATE CFG_TUH_RPI_PIO_USB=1)
target_link_libraries(gip_hostdrv
        pico_stdlib
        pico_multicore
        tinyusb_host
        pico_pio_usb
        hardware_i2c
        hardware_dma
)
pico_add_extra_outputs(gip_hostdrv)

# ---- Passthrough relay: DualSense/controller -> Xbox (device + host + relay) ----
add_executable(passthrough
        src/passthru/passthru.cpp
        src/xbox/xbox_descriptors.c
        src/gipdrv/gip_host.c
        src/bringup/ssd1306.cpp
        ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)
target_include_directories(passthrough PRIVATE src/passthru src/gipdrv src/bringup ${PICO_TINYUSB_PATH}/src)
pico_set_program_name(passthrough "passthrough")
pico_enable_stdio_uart(passthrough 1)
pico_enable_stdio_usb(passthrough 0)
target_compile_definitions(passthrough PRIVATE CFG_TUH_RPI_PIO_USB=1)
target_link_libraries(passthrough
        pico_stdlib
        pico_multicore
        tinyusb_device
        tinyusb_host
        pico_pio_usb
        hardware_i2c
        hardware_dma
)
pico_add_extra_outputs(passthrough)

# ---- Xbox controller DEVICE spoof (enumerate as Xbox pad + capture host GIP) ----
add_executable(xbox_dev
        src/xbox/xbox_dev.cpp
        src/xbox/xbox_descriptors.c
        src/bringup/ssd1306.cpp
)
target_include_directories(xbox_dev PRIVATE src/xbox src/bringup)
pico_set_program_name(xbox_dev "xbox_dev")
pico_enable_stdio_uart(xbox_dev 1)
pico_enable_stdio_usb(xbox_dev 0)
target_link_libraries(xbox_dev
        pico_stdlib
        tinyusb_device
        tinyusb_board
        hardware_i2c
)
pico_add_extra_outputs(xbox_dev)


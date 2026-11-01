cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(APP_NAME "mdac_adc2")
set(BOARD_NAME "rp2040" CACHE STRING "Target SoC: rp2040 or rp2350")

# Existing DAC output pin assignment.
set(PICODAC_I2S_DATA_PIN 22 CACHE STRING "I2S transmit DATA pin")
set(PICODAC_I2S_BASE_CLOCK_PIN 20 CACHE STRING "I2S BCLK pin; LRCLK is BASE + 1")
# Separate digital I2S input pin. BCLK/LRCLK are shared with the DAC TX PIO.
set(PICODAC_I2S_RX_DATA_PIN 23 CACHE STRING "I2S receive DATA pin")
set(PICODAC_VENDOR_ID 0xcafe CACHE STRING "USB Vendor ID")
set(PICODAC_PRODUCT_ID 0xbabe CACHE STRING "USB Product ID")

if(BOARD_NAME STREQUAL "rp2040")
    set(_default_pico_board pico)
elseif(BOARD_NAME STREQUAL "rp2350")
    set(_default_pico_board pico2)
else()
    message(FATAL_ERROR "Unsupported BOARD_NAME='${BOARD_NAME}'. Use rp2040 or rp2350.")
endif()

# Allow an explicit board variant such as pico2_w from the command line.
# Use a fresh build directory when changing silicon families so stale cache
# values from the previous target cannot be reused.
set(PICO_BOARD "${_default_pico_board}" CACHE STRING "Pico SDK board type")
message(STATUS "Target SoC: ${BOARD_NAME}; Pico SDK board: ${PICO_BOARD}")

if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if(EXISTS ${picoVscode})
    include(${picoVscode})
endif()

include(pico_sdk_import.cmake)
project(mdac_adc2 C CXX ASM)
pico_sdk_init()

add_executable(mdac_adc2
    main.c
    audio_device.c
    audio_capture.c
    blink.c
    i2s.c
    i2s_rx.c
    ringbuffer.c
    usb.c
    usb_audio.c
    usb_audio_capture.c
    usb_hid.c
)

target_link_libraries(mdac_adc2
    hardware_dma
    hardware_pio
    pico_stdlib
)

set(_mdac_compile_definitions
    PICODAC_I2S_DATA_PIN=${PICODAC_I2S_DATA_PIN}
    PICODAC_I2S_BASE_CLOCK_PIN=${PICODAC_I2S_BASE_CLOCK_PIN}
    PICODAC_I2S_RX_DATA_PIN=${PICODAC_I2S_RX_DATA_PIN}
    HID_ENABLE=1
    VENDOR_ID=${PICODAC_VENDOR_ID}
    PRODUCT_ID=${PICODAC_PRODUCT_ID}
)

# The original custom 92.16 MHz PLL setup is RP2040-specific. Do not raise
# RP2350's documented PLL VCO limit to accommodate that RP2040 configuration.
if(BOARD_NAME STREQUAL "rp2040")
    list(APPEND _mdac_compile_definitions PICO_PLL_VCO_MAX_FREQ_HZ=2304000000)
endif()

# Pico 2 W has no directly accessible PICO_DEFAULT_LED_PIN; its status LED is
# attached to the CYW43 wireless chip. This firmware does not initialize the
# CYW43, so disable the PIO-based status LED rather than driving GPIO25 (the
# CYW43 chip-select pin) as though it were an ordinary onboard LED.
if(PICO_BOARD STREQUAL "pico2_w")
    list(APPEND _mdac_compile_definitions PICODAC_NO_PIO_LED=1)
endif()

target_compile_definitions(mdac_adc2 PRIVATE ${_mdac_compile_definitions})
target_include_directories(mdac_adc2 PRIVATE ${CMAKE_CURRENT_LIST_DIR})

pico_generate_pio_header(mdac_adc2
    ${CMAKE_CURRENT_LIST_DIR}/blink.pio
    ${CMAKE_CURRENT_LIST_DIR}/i2s.pio
    ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio
)

pico_set_program_name(mdac_adc2 "mdac_adc2")
pico_set_program_version(mdac_adc2 "0.3")
pico_enable_stdio_uart(mdac_adc2 1)
pico_enable_stdio_usb(mdac_adc2 0)
pico_add_extra_outputs(mdac_adc2)

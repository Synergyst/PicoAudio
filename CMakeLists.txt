cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(APP_NAME "mdac_adc2")
set(BOARD_NAME "rp2350" CACHE STRING "Target SoC: rp2350 (Pico 2 / Pico 2 W)")
set(PICODAC_LINK_ROLE "master" CACHE STRING "GPIO bitbang link role: master or slave")
set_property(CACHE PICODAC_LINK_ROLE PROPERTY STRINGS master slave)
set(PICODAC_VENDOR_ID 0xcafe CACHE STRING "USB Vendor ID")
set(PICODAC_PRODUCT_ID 0xbabe CACHE STRING "USB Product ID")

if(BOARD_NAME STREQUAL "rp2350")
    set(_default_pico_board pico2)
else()
    message(FATAL_ERROR "Unsupported BOARD_NAME='${BOARD_NAME}'. This C-only GPIO diagnostic build requires RP2350 (pico2 or pico2w).")
endif()

if(NOT PICODAC_LINK_ROLE STREQUAL "master" AND NOT PICODAC_LINK_ROLE STREQUAL "slave")
    message(FATAL_ERROR "Unsupported PICODAC_LINK_ROLE='${PICODAC_LINK_ROLE}'. Use master or slave.")
endif()

set(PICO_BOARD "${_default_pico_board}" CACHE STRING "Pico SDK board type")
message(STATUS "Target SoC: ${BOARD_NAME}; Pico SDK board: ${PICO_BOARD}; GPIO bitbang role: ${PICODAC_LINK_ROLE}")

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
    ringbuffer.c
    gpio_audio_link.c
    usb.c
    usb_audio.c
    usb_audio_capture.c
    usb_hid.c
)

target_link_libraries(mdac_adc2
    hardware_pio
    pico_stdlib
    pico_multicore
)

set(_mdac_compile_definitions
    HID_ENABLE=1
    VENDOR_ID=${PICODAC_VENDOR_ID}
    PRODUCT_ID=${PICODAC_PRODUCT_ID}
)

if(PICODAC_LINK_ROLE STREQUAL "master")
    list(APPEND _mdac_compile_definitions PICODAC_GPIO_LINK_ROLE_MASTER=1)
else()
    list(APPEND _mdac_compile_definitions PICODAC_GPIO_LINK_ROLE_SLAVE=1)
endif()

if(PICO_BOARD STREQUAL "pico2_w")
    list(APPEND _mdac_compile_definitions PICODAC_NO_PIO_LED=1)
endif()

target_compile_definitions(mdac_adc2 PRIVATE ${_mdac_compile_definitions})
target_include_directories(mdac_adc2 PRIVATE ${CMAKE_CURRENT_LIST_DIR})

pico_generate_pio_header(mdac_adc2
    ${CMAKE_CURRENT_LIST_DIR}/blink.pio
)

pico_set_program_name(mdac_adc2 "mdac_adc2")
pico_set_program_version(mdac_adc2 "0.8-gpio-c-diagnostic")
pico_enable_stdio_uart(mdac_adc2 1)
pico_enable_stdio_usb(mdac_adc2 0)
pico_add_extra_outputs(mdac_adc2)

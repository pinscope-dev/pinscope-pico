# Pinscope on a Raspberry Pi Pico 2 W:
#
#   PINSCOPE_KEY=psk_... WIFI_SSID=... WIFI_PASSWORD=... cmake -S sample -B build
#   cmake --build build -j        # then copy build/pinscope_pico.uf2 to the board
#
# Needs CMake, Ninja or make, and the arm-none-eabi GCC. Without PICO_SDK_PATH
# and FREERTOS_KERNEL_PATH, the Pico SDK 2.3.1 and the FreeRTOS kernel are
# downloaded into the build directory (a few hundred MB, once).
cmake_minimum_required(VERSION 3.17)
set(PICO_BOARD pico2_w CACHE STRING "")
include(FetchContent)
if(NOT PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
  set(PICO_SDK_FETCH_FROM_GIT ON)
  set(PICO_SDK_FETCH_FROM_GIT_TAG 2.3.1)
endif()
include(pico_sdk_import.cmake)
if(NOT FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH} AND NOT EXISTS ${PICO_SDK_PATH}/../FreeRTOS-Kernel)
  FetchContent_Declare(freertos_kernel GIT_REPOSITORY https://github.com/raspberrypi/FreeRTOS-Kernel.git
    GIT_TAG 4f7299d6ea746b27a9dd19e87af568e34bd65b15 GIT_SUBMODULES_RECURSE FALSE)
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD) # populate only: the Pico port's import adds the kernel itself
  endif()
  FetchContent_Populate(freertos_kernel)
  set(FREERTOS_KERNEL_PATH ${freertos_kernel_SOURCE_DIR})
endif()
include(FreeRTOS_Kernel_import.cmake)
# The key and the WiFi network: -D on the command line, or the environment.
foreach(var PINSCOPE_KEY WIFI_SSID WIFI_PASSWORD)
  if(NOT DEFINED ${var} AND DEFINED ENV{${var}})
    set(${var} $ENV{${var}})
  endif()
endforeach()
if(NOT PINSCOPE_KEY)
  message(FATAL_ERROR "No project key. Copy it from the dashboard (Connect a device) and run cmake with PINSCOPE_KEY=psk_... in the environment, or -DPINSCOPE_KEY=psk_...")
endif()
if(NOT WIFI_SSID)
  message(FATAL_ERROR "No WiFi network: set WIFI_SSID and WIFI_PASSWORD in the environment, or pass -DWIFI_SSID=... -DWIFI_PASSWORD=...")
endif()
project(pinscope_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()
include(${CMAKE_CURRENT_LIST_DIR}/../pinscope.cmake)

add_executable(pinscope_pico main.c)
target_include_directories(pinscope_pico PRIVATE ${CMAKE_CURRENT_LIST_DIR}) # lwipopts.h, FreeRTOSConfig.h
target_compile_definitions(pinscope_pico PRIVATE
  WIFI_SSID=\"${WIFI_SSID}\"
  WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
  PINSCOPE_KEY=\"${PINSCOPE_KEY}\"
)
target_link_libraries(pinscope_pico pinscope pico_stdlib pico_cyw43_arch_lwip_sys_freertos FreeRTOS-Kernel-Heap4)
pico_enable_stdio_usb(pinscope_pico 1)
pico_enable_stdio_uart(pinscope_pico 0)
pico_add_extra_outputs(pinscope_pico)

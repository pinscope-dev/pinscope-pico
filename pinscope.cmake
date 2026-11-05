# Pinscope for the Raspberry Pi Pico SDK (Pico 2 W). In a project:
#
#   include(<pinscope>/ports/pico/pinscope.cmake)     after pico_sdk_init()
#   target_link_libraries(app pinscope pico_cyw43_arch_lwip_sys_freertos FreeRTOS-Kernel-Heap4 ...)
#
# and in code, once WiFi is up:  pinscope_start(&(pinscope_config_t){ .project_key = "psk_..." });
# lwipopts.h needs NO_SYS 0, LWIP_SOCKET 1 and LWIP_NETIF_LOOPBACK 1; the build says so if not.
set(PINSCOPE_PICO_DIR ${CMAKE_CURRENT_LIST_DIR})
# In the public library the core is in core/; in this repository two levels up.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/core/ps_core.c)
  set(PINSCOPE_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/core)
else()
  set(PINSCOPE_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../device/pinscope/src)
endif()

# An INTERFACE library, like the SDK's own, so it builds with the project's
# lwipopts.h and FreeRTOSConfig.h.
add_library(pinscope INTERFACE)
target_sources(pinscope INTERFACE
  ${PINSCOPE_CORE_DIR}/ps_core.c
  ${PINSCOPE_CORE_DIR}/ps_crypto.c
  ${PINSCOPE_PICO_DIR}/ps_port_pico.c)
target_include_directories(pinscope INTERFACE ${PINSCOPE_CORE_DIR})
# PICO_CRT0_IMAGE_TYPE_TBYB: an updated image boots on trial ("try before you
# buy") and the bootrom goes back unless Pinscope confirms it once connected.
target_compile_definitions(pinscope INTERFACE PS_LWIP_SOCKETS=1 PICO_CRT0_IMAGE_TYPE_TBYB=1)
target_link_libraries(pinscope INTERFACE
  pico_unique_id pico_flash hardware_adc boot_uf2_headers boot_picoboot_headers)
# The GNU build ID in flash, read by the firmware and by the server (build_id.ld).
target_link_options(pinscope INTERFACE -Wl,--build-id=sha1 -Wl,-T,${PINSCOPE_PICO_DIR}/build_id.ld)

# Pinscope for the Raspberry Pi Pico 2 W

Remote access, logs, metrics, HardFault reports and A/B firmware updates for the
Pico 2 W with the Pico SDK, over one outbound connection to
[Pinscope](https://pinscope.dev). About 9 KB of RAM, no TLS library.

## Try it in five minutes

You need CMake, Ninja and the Arm GCC (`arm-none-eabi-gcc`; on macOS
`brew install --cask gcc-arm-embedded`, on Debian or Ubuntu
`sudo apt install gcc-arm-none-eabi`), and the project key from **Connect a
device** in the [dashboard](https://pinscope.dev/#/).

```sh
git clone https://github.com/pinscope-dev/pinscope-pico && cd pinscope-pico
PINSCOPE_KEY=psk_... WIFI_SSID=... WIFI_PASSWORD=... cmake -S sample -B build -G Ninja
cmake --build build
```

Hold BOOTSEL while plugging the board in and copy `build/pinscope_pico.uf2` to
it. Without `PICO_SDK_PATH`, the Pico SDK 2.3.1 and the FreeRTOS kernel are
downloaded into `build/` the first time.

For updates, the board needs the A/B partition table once:
`picotool partition create sample/partitions.json pt.uf2`, copy `pt.uf2` to the
board, then the sample. Upload `pinscope_pico.uf2` under Firmware.

## Use it in your project

Clone this repository next to your project, then in `CMakeLists.txt`, after
`pico_sdk_init()`:

```cmake
include(pinscope-pico/pinscope.cmake)
target_link_libraries(app pinscope pico_cyw43_arch_lwip_sys_freertos FreeRTOS-Kernel-Heap4)
```

and once WiFi is connected:

```c
#include "pinscope.h"

pinscope_start(&(pinscope_config_t){ .project_key = "psk_..." });
```

`lwipopts.h` needs `NO_SYS 0`, `LWIP_SOCKET 1` and `LWIP_NETIF_LOOPBACK 1`; the
build says so if not. `printf` output becomes the device log.

Documentation: https://pinscope.dev/docs/pico

## Licence

Source-available: the licence in `LICENSE` allows use in any product, including
commercial ones, for connecting to [Pinscope](https://pinscope.dev). Questions:
hello@pinscope.dev.

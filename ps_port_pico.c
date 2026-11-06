/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 Pinscope. See LICENSE: for use with the Pinscope service only. */
/*
 * Pinscope platform layer for the Raspberry Pi Pico 2 W (RP2350 + CYW43):
 * Pico SDK, FreeRTOS and lwIP sockets. Updates go to the other of two A/B
 * partitions and boot "try before you buy", so the bootrom returns to the
 * previous image unless the new one is confirmed. Internal, not in the
 * public component.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "boot/bootrom_constants.h"
#include "boot/picobin.h"
#include "boot/picoboot_constants.h"
#include "boot/uf2.h"
#include "hardware/adc.h"
#include "hardware/flash.h"
#include "hardware/structs/trng.h"
#include "hardware/watchdog.h"
#include "pico/time.h"
#include "pico/bootrom.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "pico/stdio/driver.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "pinscope.h"
#include "ps_port.h"

/* Pinscope uses lwIP's sockets under FreeRTOS (pico_cyw43_arch_lwip_sys_freertos),
 * and reaches the device's own web server through lwIP's loopback. */
#if NO_SYS || !LWIP_SOCKET
#error "Pinscope needs lwIP sockets: in lwipopts.h set NO_SYS 0 and LWIP_SOCKET 1, and link pico_cyw43_arch_lwip_sys_freertos"
#endif
#if !LWIP_NETIF_LOOPBACK
#error "Pinscope reaches the local web server through lwIP's loopback: set LWIP_NETIF_LOOPBACK 1 in lwipopts.h"
#endif
#if MEMP_NUM_NETCONN < 6
#error "Pinscope needs MEMP_NUM_NETCONN of at least 6 in lwipopts.h: the tunnel, a local server, the stream to it, DNS"
#endif

static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static bool              s_capture;
static char              s_reset[12] = "poweron";

/* ---- what survives a reset: RAM the startup code leaves alone ---- */

#define CRASH_MAGIC   0x50534352u
#define RESTART_MAGIC 0x50535253u
#define CRASH_FRAMES 8
typedef struct {
    uint32_t magic;
    uint32_t count;            /* addresses in bt[] */
    uint32_t bt[CRASH_FRAMES]; /* pc, lr, then code addresses found on the stack */
    uint32_t restart;          /* RESTART_MAGIC: we restarted on purpose */
    char     build[17];        /* build ID of the image that crashed */
} crash_t;
static crash_t __uninitialized_ram(s_crash);

/* ---- build ID: the GNU note the linker put in flash (build_id.ld) ---- */

extern const uint8_t __ps_build_id_start[], __ps_build_id_end[];

/* The first `chars` hex digits of the build ID, "" when the image has none.
 * The note is namesz, descsz, type, "GNU\0", then the ID itself. */
static void build_id_hex(char *out, size_t chars)
{
    const uint8_t *note = __ps_build_id_start;
    out[0] = '\0';
    if ((size_t)(__ps_build_id_end - __ps_build_id_start) < 16 + 8) {
        return;
    }
    uint32_t namesz, descsz;
    memcpy(&namesz, note, 4);
    memcpy(&descsz, note + 4, 4);
    const uint8_t *id = note + 12 + ((namesz + 3) & ~3u);
    for (size_t i = 0; i < descsz && 2 * i + 2 <= chars; i++) {
        snprintf(out + 2 * i, 3, "%02x", id[i]);
    }
}

extern char __logical_binary_start[], __etext[];

static bool in_code(uint32_t address)
{
    return address >= (uint32_t)(uintptr_t)__logical_binary_start && address < (uint32_t)(uintptr_t)__etext;
}

/* HardFault: keep where it happened and what the stack points to, then
 * reset. The watchdog reboot keeps the uninitialised RAM. */
void ps_fault(uint32_t *frame)
{
    s_crash.magic = CRASH_MAGIC;
    s_crash.count = 0;
    /* Thumb addresses have bit 0 set; the ELF symbols do not. */
    s_crash.bt[s_crash.count++] = frame[6] & ~1u;
    s_crash.bt[s_crash.count++] = frame[5] & ~1u;
    for (const uint32_t *sp = frame + 8; (uintptr_t)(sp + 1) <= SRAM_END && sp < frame + 72 &&
                                         s_crash.count < CRASH_FRAMES; sp++) {
        if ((*sp & 1u) && in_code(*sp & ~1u)) {
            s_crash.bt[s_crash.count++] = *sp & ~1u;
        }
    }
    build_id_hex(s_crash.build, 16);
    watchdog_reboot(0, 0, 0);
    for (;;) {
    }
}

__attribute__((naked)) void isr_hardfault(void)
{
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "b ps_fault      \n");
}

/* ---- keys need real randomness: the RP2350's TRNG ---- */

static uint32_t trng_word(void)
{
    for (;;) {
        trng_hw->rng_icr = -1u;
        trng_hw->rnd_source_enable = 1;
        while (!(trng_hw->trng_valid & TRNG_TRNG_VALID_EHR_VALID_BITS)) {
            tight_loop_contents();
        }
        uint32_t w = trng_hw->ehr_data[0];
        /* ehr_data[5] read last empties the buffer for the next sample */
        (void)trng_hw->ehr_data[5];
        if (!(trng_hw->rng_isr & (TRNG_RNG_ISR_AUTOCORR_ERR_BITS | TRNG_RNG_ISR_CRNGT_ERR_BITS))) {
            return w;
        }
    }
}

void ps_port_random(void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        uint32_t w = trng_word();
        size_t n = len < 4 ? len : 4;
        memcpy(p, &w, n);
        p += n;
        len -= n;
    }
}

int ps_port_init(void)
{
    if (s_lock) {
        return 0;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_crash.magic == CRASH_MAGIC) {
        strcpy(s_reset, "panic");
    } else if (s_crash.restart == RESTART_MAGIC) {
        strcpy(s_reset, "sw");
    } else if (watchdog_caused_reboot()) {
        strcpy(s_reset, "wdt");
    }
    s_crash.restart = 0;
    adc_init();
    adc_set_temp_sensor_enabled(true);
    return s_lock ? 0 : -1;
}

uint32_t ps_port_millis(void) { return to_ms_since_boot(get_absolute_time()); }
void     ps_port_sleep(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
void     ps_port_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void     ps_port_unlock(void) { xSemaphoreGive(s_lock); }

int ps_port_task_start(void (*fn)(void *), uint32_t stack, uint8_t priority)
{
    (void)priority;
    return xTaskCreate(fn, "pinscope", stack / sizeof(StackType_t), NULL, tskIDLE_PRIORITY + 2, &s_task) == pdPASS ? 0 : -1;
}

void ps_port_logf(int level, const char *fmt, ...)
{
    static const char *names[] = { "E", "W", "I" };
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s pinscope: %s\n", names[level], line); /* reaches Pinscope through the stdio driver */
}

void ps_port_device_id(char *buf, size_t len) { pico_get_unique_board_id_string(buf, (uint)len); }

/* A new value on every start: the server counts a changed one as a restart. */
static uint32_t boot_id(void)
{
    static uint32_t id;
    if (!id) {
        ps_port_random(&id, sizeof(id));
        id |= 1;
    }
    return id;
}

void ps_port_info(char *buf, size_t len)
{
    char build[17];
    build_id_hex(build, 16);
    snprintf(buf, len, "chip=rp2350;board=pico2_w;fw=;sdk=%s;reset=%s;elf=%s;boot=%08x", PICO_SDK_VERSION_STRING, s_reset,
             build, (unsigned)boot_id());
}

/* ---- small persistent store: the last flash sector, "key=value" lines ---- */

#define KV_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define KV_TEXT   ((const char *)(XIP_BASE + KV_OFFSET))

int ps_port_kv_get(const char *key, char *buf, size_t len)
{
    size_t k = strlen(key);
    const char *p = KV_TEXT, *end = KV_TEXT + FLASH_SECTOR_SIZE;
    while (p < end && *p != (char)0xff && *p) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) {
            break;
        }
        if ((size_t)(nl - p) > k && !memcmp(p, key, k) && p[k] == '=') {
            size_t n = (size_t)(nl - p - (ptrdiff_t)k - 1);
            if (n >= len) {
                return -1;
            }
            memcpy(buf, p + k + 1, n);
            buf[n] = '\0';
            return 0;
        }
        p = nl + 1;
    }
    return -1;
}

static uint8_t s_sector[FLASH_SECTOR_SIZE];

static void kv_write(void *arg)
{
    (void)arg;
    flash_range_erase(KV_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(KV_OFFSET, s_sector, FLASH_SECTOR_SIZE);
}

/* Rewrites the sector with this key changed; the other keys are kept. */
int ps_port_kv_set(const char *key, const char *value)
{
    size_t k = strlen(key), used = 0;
    const char *p = KV_TEXT, *end = KV_TEXT + FLASH_SECTOR_SIZE;
    memset(s_sector, 0xff, sizeof(s_sector));
    while (p < end && *p != (char)0xff && *p) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) {
            break;
        }
        size_t line = (size_t)(nl - p) + 1;
        if (!(line > k + 1 && !memcmp(p, key, k) && p[k] == '=') && used + line <= sizeof(s_sector)) {
            memcpy(s_sector + used, p, line);
            used += line;
        }
        p = nl + 1;
    }
    int n = snprintf((char *)s_sector + used, sizeof(s_sector) - used, "%s=%s\n", key, value);
    if (n < 0 || used + (size_t)n >= sizeof(s_sector)) {
        return -1;
    }
    s_sector[used + (size_t)n] = 0xff; /* snprintf's NUL is not part of the store */
    return flash_safe_execute(kv_write, NULL, 1000) == PICO_OK ? 0 : -1;
}

/* Everything printed with printf goes to Pinscope too, a line at a time. In an
 * interrupt, or when Pinscope itself holds the lock, the line is skipped. */
static char   s_line[160];
static size_t s_line_len;

static void capture_chars(const char *buf, int len)
{
    if (!s_capture || portCHECK_IF_IN_ISR()) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\r') {
            continue;
        }
        if (s_line_len < sizeof(s_line) - 1) {
            s_line[s_line_len++] = buf[i];
        }
        if (buf[i] == '\n' || s_line_len == sizeof(s_line) - 1) {
            ps_log_locked(s_line, s_line_len);
            s_line_len = 0;
        }
    }
    xSemaphoreGive(s_lock);
}

static stdio_driver_t s_capture_driver = { .out_chars = capture_chars };

void ps_port_log_capture(bool on)
{
    s_capture = on;
    stdio_set_driver_enabled(&s_capture_driver, on);
}

int ps_port_crash_read(char *buf, size_t len)
{
    if (s_crash.magic != CRASH_MAGIC || s_crash.count > CRASH_FRAMES) {
        return 0;
    }
    s_crash.build[sizeof(s_crash.build) - 1] = '\0';
    int n = snprintf(buf, len, "crash\nreason=HardFault\ntask=%s\npc=0x%08x\napprox=1\nelf=%s\nbt=", "?",
                     (unsigned)s_crash.bt[0], s_crash.build);
    for (uint32_t i = 0; i < s_crash.count && n > 0 && (size_t)n < len; i++) {
        n += snprintf(buf + n, len - (size_t)n, i ? " 0x%08x" : "0x%08x", (unsigned)s_crash.bt[i]);
    }
    if (n > 0 && (size_t)n < len - 1) {
        buf[n++] = '\n';
        buf[n] = '\0';
    }
    return n;
}

void ps_port_crash_clear(void) { s_crash.magic = 0; }

/* ---- updates: a UF2 file, written block by block to the other partition ---- */

static struct {
    bool     open;
    uint8_t  block[512];
    uint16_t have;
    uint32_t family, blocks, done;
    uint32_t offset;     /* added to a block's target address: where it goes in flash */
    uint32_t first, last; /* flash offsets of the target partition */
    uint32_t erased;     /* the next flash offset not erased yet */
    uint32_t boot_addr;  /* XIP address of the new image, for the reboot */
} s_ota;

static __attribute__((aligned(4))) uint8_t s_workarea[3264]; /* bootrom scratch */

/* Updates need the A/B partition table (picotool partition create); without
 * one the bootrom has nowhere to put a second image. */
bool ps_port_ota_supported(void)
{
    resident_partition_t part;
    rom_flash_flush_cache();
    return rom_get_uf2_target_partition(s_workarea, sizeof(s_workarea), 0xe48bff59u /* rp2350-arm-s */, &part) >= 0;
}

int ps_port_ota_begin(uint32_t size)
{
    (void)size;
    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.open = true;
    return 0;
}

static void ota_flash(void *arg)
{
    const struct uf2_block *b = arg;
    uint32_t at = b->target_addr + s_ota.offset - XIP_BASE;
    if (at >= s_ota.erased) {
        uint32_t sector = at & ~(FLASH_SECTOR_SIZE - 1);
        flash_range_erase(sector, FLASH_SECTOR_SIZE);
        s_ota.erased = sector + FLASH_SECTOR_SIZE;
    }
    flash_range_program(at, b->data, 256);
}

static int ota_block(const struct uf2_block *b)
{
    if (b->magic_start0 != UF2_MAGIC_START0 || b->magic_start1 != UF2_MAGIC_START1 ||
        b->magic_end != UF2_MAGIC_END || b->payload_size != 256) {
        return -1;
    }
    /* RP2350 UF2 files start with a block in the "absolute" family, a
     * workaround for erratum RP2350-E10; it is not part of the image. */
    if (b->file_size == 0xe48bff57u) {
        return 0;
    }
    if (s_ota.blocks == 0) {
        resident_partition_t part;
        s_ota.family = b->file_size;
        s_ota.blocks = b->num_blocks;
        rom_flash_flush_cache();
        if (rom_get_uf2_target_partition(s_workarea, sizeof(s_workarea), s_ota.family, &part) < 0) {
            return -1;
        }
        s_ota.first = ((part.permissions_and_location & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
                       PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB) * FLASH_SECTOR_SIZE;
        s_ota.last = (((part.permissions_and_location & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >>
                       PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB) + 1) * FLASH_SECTOR_SIZE;
        s_ota.boot_addr = XIP_BASE + s_ota.first;
        s_ota.offset = XIP_BASE + s_ota.first - b->target_addr;
        s_ota.erased = s_ota.first;
    }
    uint32_t at = b->target_addr + s_ota.offset - XIP_BASE;
    if (b->block_no != s_ota.done || b->file_size != s_ota.family || at < s_ota.first || at + 256 > s_ota.last) {
        return -1;
    }
    if (flash_safe_execute(ota_flash, (void *)b, 1000) != PICO_OK) {
        return -1;
    }
    s_ota.done++;
    return 0;
}

int ps_port_ota_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    while (s_ota.open && len) {
        size_t n = sizeof(s_ota.block) - s_ota.have;
        n = n < len ? n : len;
        memcpy(s_ota.block + s_ota.have, p, n);
        s_ota.have = (uint16_t)(s_ota.have + n);
        p += n;
        len -= n;
        if (s_ota.have == sizeof(s_ota.block)) {
            s_ota.have = 0;
            if (ota_block((const struct uf2_block *)s_ota.block) != 0) {
                s_ota.open = false;
            }
        }
    }
    return s_ota.open ? 0 : -1;
}

/* Every block arrived: the next boot is a flash-update boot into it. */
int ps_port_ota_end(void)
{
    bool ok = s_ota.open && s_ota.have == 0 && s_ota.blocks && s_ota.done == s_ota.blocks;
    s_ota.open = false;
    return ok ? 0 : -1;
}

void ps_port_ota_abort(void) { s_ota.open = false; }

/* A new image boots under the bootrom's watchdog and has 16.7 s to buy itself
 * (RP2350 datasheet, 5.1.17), less than joining WiFi and reaching the server
 * can take. So the trial is ours: from before main(), a timer interrupt feeds
 * the watchdog until the image is bought or the trial is over, and then stops,
 * and the bootrom goes back to the previous image. An interrupt keeps feeding
 * while threads hang, but never past the trial. */
#ifndef CONFIG_PINSCOPE_OTA_TRIAL_S
#define CONFIG_PINSCOPE_OTA_TRIAL_S 300
#endif
static repeating_timer_t s_trial_timer;
static volatile bool s_bought;
static absolute_time_t s_trial_end;

static bool trial_feed(repeating_timer_t *t)
{
    (void)t;
    if (s_bought || absolute_time_diff_us(get_absolute_time(), s_trial_end) <= 0) {
        return false;
    }
    watchdog_update();
    return true;
}

__attribute__((constructor)) static void trial_watch(void)
{
    if (!ps_port_ota_on_trial()) {
        return;
    }
    s_trial_end = make_timeout_time_ms(CONFIG_PINSCOPE_OTA_TRIAL_S * 1000u);
    watchdog_enable(8000, true);
    add_repeating_timer_ms(1000, trial_feed, NULL, &s_trial_timer);
}

void ps_port_ota_confirm(void)
{
    if (rom_get_last_boot_type() == BOOT_TYPE_FLASH_UPDATE &&
        rom_explicit_buy(s_workarea, sizeof(s_workarea)) == BOOTROM_OK) {
        s_bought = true;
        cancel_repeating_timer(&s_trial_timer);
        watchdog_disable();
    }
}

bool ps_port_ota_on_trial(void)
{
    boot_info_t info = { 0 };
    return rom_get_last_boot_type() == BOOT_TYPE_FLASH_UPDATE && rom_get_boot_info(&info) &&
           (info.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING);
}

void ps_port_restart(void)
{
    s_crash.restart = RESTART_MAGIC;
    if (s_ota.boot_addr && s_ota.done && s_ota.done == s_ota.blocks) {
        rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 100, s_ota.boot_addr, 0);
    } else {
        watchdog_reboot(0, 0, 100);
    }
    for (;;) {
        vTaskDelay(1);
    }
}

/* ---- system metrics ---- */

uint16_t ps_port_default_metrics(void)
{
    return PINSCOPE_M_HEAP | PINSCOPE_M_UPTIME | PINSCOPE_M_RSSI | PINSCOPE_M_TEMP | PINSCOPE_M_STACK;
}

void ps_port_sys_metrics(uint16_t mask)
{
    if (mask & PINSCOPE_M_UPTIME) {
        pinscope_metric("uptime_s", (double)(ps_port_millis() / 1000));
    }
    if (mask & PINSCOPE_M_HEAP) {
        pinscope_metric("heap_free", (double)xPortGetFreeHeapSize());
        pinscope_metric("heap_min", (double)xPortGetMinimumEverFreeHeapSize());
    }
    if (mask & PINSCOPE_M_STACK) {
        pinscope_metric("stack/pinscope", (double)(uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t)));
    }
    int32_t rssi;
    if ((mask & PINSCOPE_M_RSSI) && cyw43_wifi_get_rssi(&cyw43_state, &rssi) == 0) {
        pinscope_metric("rssi", (double)rssi);
    }
    if (mask & PINSCOPE_M_TEMP) {
        /* The on-chip sensor is the last ADC input: 27 °C at 0.706 V, -1.721 mV/°C. */
        adc_select_input(NUM_ADC_CHANNELS - 1);
        float volts = adc_read() * 3.3f / 4096;
        pinscope_metric("chip_temp_c", (double)(27 - (volts - 0.706f) / 0.001721f));
    }
}

/* ---- connections to local servers: lwIP sockets ---- */

int ps_local_open(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ps_set_nonblock(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    /* The device's own address, which lwIP loops back (LWIP_NETIF_LOOPBACK). */
    const ip4_addr_t *own = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
    addr.sin_addr.s_addr = own && !ip4_addr_isany(own) ? ip4_addr_get_u32(own) : htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 && !ps_sock_inprogress()) {
        lwip_close(fd);
        return -1;
    }
    return fd;
}

int ps_local_state(int h)
{
    fd_set w;
    struct timeval tv = { 0, 0 };
    int err = 0;
    socklen_t len = sizeof(err);
    FD_ZERO(&w);
    FD_SET(h, &w);
    if (select(h + 1, NULL, &w, NULL, &tv) <= 0) {
        return 0;
    }
    getsockopt(h, SOL_SOCKET, SO_ERROR, &err, &len);
    return err == 0 ? 1 : -1;
}

int ps_local_read(int h, void *buf, size_t len)
{
    int n = (int)recv(h, buf, len, 0);
    if (n > 0) {
        return n;
    }
    return (n < 0 && ps_sock_wouldblock()) ? 0 : -1;
}

int ps_local_write(int h, const void *buf, size_t len)
{
    int n = (int)send(h, buf, len, 0);
    if (n > 0) {
        return n;
    }
    return (n < 0 && ps_sock_wouldblock()) ? 0 : -1;
}

int  ps_local_avail(int h) { (void)h; return -1; }
void ps_local_want(int h, size_t bytes) { (void)h; (void)bytes; }
void ps_local_close(int h) { lwip_close(h); }

void ps_local_watch(int h, bool connecting, bool want_read, bool want_write,
                    fd_set *r, fd_set *w, fd_set *e, int *maxfd)
{
    if (connecting) {
        FD_SET(h, w);
        FD_SET(h, e);
    }
    if (want_read) {
        FD_SET(h, r);
    }
    if (want_write) {
        FD_SET(h, w);
    }
    if (h > *maxfd) {
        *maxfd = h;
    }
}

void ps_local_flush(void) {}
bool ps_local_ready(void) { return false; }
void ps_local_poll(const fd_set *readable) { (void)readable; }

bool ps_port_network_ready(void) { return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP; }

int ps_port_resolve(const char *host, uint8_t *scratch, size_t len, uint32_t *addr)
{
    struct addrinfo hints, *res = NULL;
    (void)scratch;
    (void)len;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return -1;
    }
    *addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return 0;
}

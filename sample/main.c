/*
 * Pinscope on a Pico 2 W: a small web page on port 80, reachable through
 * the device's Pinscope address, plus metrics and logs.
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "pico/cyw43_arch.h" /* before lwIP's sockets, which define a poll() macro */
#include "pico/stdlib.h"
#include "lwip/sockets.h"

#include "pinscope.h"

static void serve(void)
{
    int server = lwip_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = PP_HTONS(80) };
    if (lwip_bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0 || lwip_listen(server, 2) != 0) {
        printf("cannot listen on port 80\n");
        return;
    }
    for (;;) {
        int client = lwip_accept(server, NULL, NULL);
        if (client < 0) {
            continue;
        }
        char request[1024], page[192], head[128];
        /* Read the whole request: closing with unread bytes resets the
         * connection, and the reply would be lost. */
        size_t got = 0;
        while (got < sizeof(request) - 1) {
            int n = (int)lwip_recv(client, request + got, sizeof(request) - 1 - got, 0);
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
            request[got] = '\0';
            if (strstr(request, "\r\n\r\n")) {
                break;
            }
        }
        int body = snprintf(page, sizeof(page), "<!doctype html><title>Pico 2 W</title><h1>Hello from a Pico 2 W</h1><p>Up for %lu s.</p>",
                            (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000));
        int n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", body);
        lwip_send(client, head, n, 0);
        lwip_send(client, page, body, 0);
        lwip_close(client);
    }
}

static void main_task(void *arg)
{
    (void)arg;
    if (cyw43_arch_init()) {
        printf("WiFi chip did not start\n");
        vTaskDelete(NULL);
    }
    cyw43_arch_enable_sta_mode();
    while (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("WiFi: not connected, trying again\n");
    }
    pinscope_start(&(pinscope_config_t){ .project_key = PINSCOPE_KEY }); /* the one line */
    serve();
}

int main(void)
{
    stdio_init_all();
    xTaskCreate(main_task, "main", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    vTaskStartScheduler();
}

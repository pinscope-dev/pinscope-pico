#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* Raspberry Pi's example settings, then what Pinscope needs: sockets with
 * select() under FreeRTOS, and loopback so a stream can reach this device's
 * own web server at its own address. */
#include "lwipopts_common.h"

#undef NO_SYS
#define NO_SYS                      0
#undef LWIP_SOCKET
#define LWIP_SOCKET                 1
#undef LWIP_NETCONN
#define LWIP_NETCONN                1
#define LWIP_POSIX_SOCKETS_IO_NAMES 0 /* no close()/read() macros over newlib's */
#define LWIP_SO_RCVTIMEO            1
#define LWIP_TIMEVAL_PRIVATE        0
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_LOOPBACK_MAX_PBUFS     16
#define MEMP_NUM_NETCONN            12
#define MEMP_NUM_TCP_PCB            12
#define LWIP_TCPIP_CORE_LOCKING_INPUT 1

#define TCPIP_THREAD_STACKSIZE      2048
#define DEFAULT_THREAD_STACKSIZE    1024
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define DEFAULT_UDP_RECVMBOX_SIZE   8
#define DEFAULT_TCP_RECVMBOX_SIZE   8
#define DEFAULT_ACCEPTMBOX_SIZE     8
#define TCPIP_MBOX_SIZE             16

#undef LWIP_DEBUG
#undef LWIP_STATS
#define LWIP_STATS                  0
#undef LWIP_STATS_DISPLAY
#define LWIP_STATS_DISPLAY          0

#endif

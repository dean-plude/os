/*
 * lwipopts.h — lwIP configuration for NovaOS
 *
 * lwIP runs "bare" (NO_SYS): one kernel thread (net_thread in net.c) owns
 * the stack, and other threads call the raw API only while holding the
 * net lock (see net.h).  Callbacks run on whichever thread holds the lock
 * and only record data.
 */
#pragma once

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_TIMERS                 1
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_PROVIDE_ERRNO          1

/* Memory: lwIP's own heap and pools (no libc malloc).  Sized for a
 * desktop's load, not a microcontroller's: a browser or a game client
 * keeps hundreds of sockets open, downloads over dozens of connections at
 * once and runs several DNS lookups in parallel, and Windows has no fixed
 * limit for any of it.  Every PCB counts while it is in TIME-WAIT or
 * closing as well (lwIP reclaims the oldest TIME-WAIT one when the pool is
 * full, never a live connection), and each lookup in flight holds a UDP
 * PCB of its own (DNS_MAX_SOURCE_PORTS), so the old 16 TCP and 8 UDP PCBs
 * ran out and socket() failed with WSAENOBUFS. */
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (1024 * 1024)  /* tcp_write's copies (TCP_SND_BUF per sending connection), UDP sends */
#define MEMP_NUM_PBUF               256
#define MEMP_NUM_TCP_PCB            512
#define MEMP_NUM_TCP_PCB_LISTEN     64
#define MEMP_NUM_TCP_SEG            1024
#define MEMP_NUM_UDP_PCB            128
#define MEMP_NUM_MLD6_GROUP         6
#define MEMP_NUM_RAW_PCB            4
#define PBUF_POOL_SIZE              512             /* received frames: in flight, queued out of order, or refused by a full ring */
#define PBUF_POOL_BUFSIZE           1536

/* Protocols */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   1
#define LWIP_IPV6_AUTOCONFIG        1       /* SLAAC from router advertisements */
#define LWIP_IPV6_MLD               1
#define LWIP_IPV6_FRAG              0
#define LWIP_IPV6_REASS             0
#define LWIP_IPV6_NUM_ADDRESSES     3       /* link-local + two from prefixes */
#define LWIP_ND6_RDNSS_MAX_DNS_SERVERS 1    /* DNS servers from RAs (after DHCPv4's: see nd6.c) */
#define LWIP_IPV6_DHCP6             0
#define LWIP_ICMP6                  1
#define LWIP_IPV6_SEND_ROUTER_SOLICIT 1
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_IGMP                   0
#define LWIP_AUTOIP                 0
#define IP_REASSEMBLY               0
#define IP_FRAG                     0
#define LWIP_ALTCP                  0

#define DNS_MAX_SERVERS             3
#define DNS_TABLE_SIZE              32      /* names cached and asked at once (a 33rd lookup in flight fails) */
#define DNS_MAX_SOURCE_PORTS        8       /* random source ports in use at once: each is a UDP PCB taken from programs */
#define DNS_MAX_NAME_LENGTH         256

/* TCP tuning */
#define TCP_MSS                     1460
#define TCP_WND                     (16 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)
#define LWIP_TCP_KEEPALIVE          0
#define SO_REUSE                    1       /* SO_REUSEADDR (Winsock setsockopt): two sockets that both ask share a port */

/* Interface */
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_LOOPBACK         1       /* 127.0.0.1, ::1 and our own addresses (the net thread polls) */
#define ETH_PAD_SIZE                0

/* Checksums in software (the driver does no offload) */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_ICMP           1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_CHECK_ICMP         1

/* Diagnostics */
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

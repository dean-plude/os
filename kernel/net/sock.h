/*
 * sock.h — Berkeley-style sockets for user programs (ws2_32), built on
 * lwIP's raw API.  All lwIP work happens on the net thread under the
 * network lock; the calls here take the lock as needed and block by
 * yielding the calling (program) thread until the state machine advances.
 *
 * Addresses and ports are network byte order, as in the Winsock API.
 * Blocking calls return -SOCK_EWOULDBLOCK on a non-blocking socket, or
 * -SOCK_* on error; the caller (um_socket.c) maps these to WSA errors.
 */
#pragma once
#include "../include/types.h"

/* Negated error codes returned by the blocking calls */
#define SOCK_EWOULDBLOCK   1
#define SOCK_ECONNRESET    2
#define SOCK_ECONNREFUSED  3
#define SOCK_ENOTCONN      4
#define SOCK_ETIMEDOUT     5
#define SOCK_EHOSTUNREACH  6
#define SOCK_EINVAL        7
#define SOCK_ENOBUFS       8
#define SOCK_EADDRINUSE    9
#define SOCK_EISCONN       10
#define SOCK_EFAULT        11
#define SOCK_ENETDOWN      12
#define SOCK_EMFILE        13
#define SOCK_ENOTSOCK      14

/* A "kill" predicate: the blocking calls poll it and return -SOCK_ETIMEDOUT
 * (cancelled) when it returns true, so a killed program does not hang. */
typedef bool (*SockCancelFn)(void *arg);

int  NetSockTcp(void);                  /* new TCP socket; -err */
int  NetSockUdp(void);                  /* new UDP socket; -err */
int  NetSockConnect(int s, UINT32 ip_be, UINT16 port_be, SockCancelFn c, void *ca);
int  NetSockSend(int s, const void *buf, int len, SockCancelFn c, void *ca);
int  NetSockRecv(int s, void *buf, int len, SockCancelFn c, void *ca);   /* 0 = closed */
int  NetSockSendTo(int s, const void *buf, int len, UINT32 ip_be, UINT16 port_be);
int  NetSockRecvFrom(int s, void *buf, int len, UINT32 *ip_be, UINT16 *port_be,
                     SockCancelFn c, void *ca);
int  NetSockBind(int s, UINT32 ip_be, UINT16 port_be);
int  NetSockListen(int s, int backlog);
int  NetSockAccept(int s, UINT32 *ip_be, UINT16 *port_be, SockCancelFn c, void *ca);
int  NetSockShutdown(int s, int how);   /* 0 recv, 1 send, 2 both */
void NetSockClose(int s);
void NetSockSetNonblock(int s, bool nb);
int  NetSockLocalName(int s, UINT32 *ip_be, UINT16 *port_be);
int  NetSockPeerName(int s, UINT32 *ip_be, UINT16 *port_be);
/* select-style readiness (no block); *rd/*wr set if ready.  ex unused. */
void NetSockPoll(int s, bool *readable, bool *writable, bool *error);
int NetSockPeek(int s, void *buf, int len, bool *closed);
bool NetSockListening(int s);

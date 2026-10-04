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
#define SOCK_EAFNOSUPPORT  15

/* A "kill" predicate: the blocking calls poll it and return -SOCK_ETIMEDOUT
 * (cancelled) when it returns true, so a killed program does not hang. */
typedef bool (*SockCancelFn)(void *arg);

/* A socket address: family 2 (AF_INET, the address in addr[0..3]) or 23
 * (AF_INET6, with its zone in scope), port and address in network order */
typedef struct {
    UINT16 family;
    UINT16 port_be;
    UINT8  addr[16];
    UINT32 scope;
} NetSockAddr;
#define NET_AF_INET   2
#define NET_AF_INET6  23

int  NetSockTcp(int family);            /* new TCP socket (NET_AF_*); -err */
int  NetSockUdp(int family);            /* new UDP socket; -err */
int  NetSockConnect(int s, const NetSockAddr *to, SockCancelFn c, void *ca);
int  NetSockSend(int s, const void *buf, int len, SockCancelFn c, void *ca);
int  NetSockRecv(int s, void *buf, int len, SockCancelFn c, void *ca);   /* 0 = closed */
int  NetSockSendTo(int s, const void *buf, int len, const NetSockAddr *to);
int  NetSockRecvFrom(int s, void *buf, int len, NetSockAddr *from, SockCancelFn c, void *ca);
int  NetSockBind(int s, const NetSockAddr *a);
int  NetSockListen(int s, int backlog);
int  NetSockAccept(int s, NetSockAddr *peer, SockCancelFn c, void *ca);
int  NetSockShutdown(int s, int how);   /* 0 recv, 1 send, 2 both */
void NetSockClose(int s);
void NetSockSetNonblock(int s, bool nb);
int  NetSockLocalName(int s, NetSockAddr *out);
int  NetSockPeerName(int s, NetSockAddr *out);
/* select-style readiness (no block); *rd/*wr set if ready.  ex unused. */
void NetSockPoll(int s, bool *readable, bool *writable, bool *error);
int NetSockPeek(int s, void *buf, int len, bool *closed);
bool NetSockListening(int s);

/* The connection table (iphlpapi's GetExtendedTcpTable): the owner is the
 * process that made or accepted the socket; state is a MIB_TCP_STATE */
typedef struct {
    NetSockAddr local, remote;
    UINT32 state;
    UINT32 owner;
} NetTcpRow;
void NetSockSetOwner(int s, UINT32 pid);
int  NetSockTcpTable(NetTcpRow *rows, int max);

/* Socket options (Winsock's setsockopt/getsockopt; ws2_32 maps the levels
 * and names onto these).  Booleans are 0/1, buffers in bytes, timeouts in
 * ms (0: none), SOCKOPT_LINGER is on | seconds << 16.  The read-only
 * ones: SOCKOPT_TYPE (1 stream, 2 datagram), SOCKOPT_ERROR (a SOCK_E*
 * code, 0: none), SOCKOPT_ACCEPTCONN. */
enum {
    SOCKOPT_RCVBUF = 1, SOCKOPT_SNDBUF, SOCKOPT_REUSEADDR, SOCKOPT_KEEPALIVE, SOCKOPT_BROADCAST,
    SOCKOPT_NODELAY, SOCKOPT_RCVTIMEO, SOCKOPT_SNDTIMEO, SOCKOPT_LINGER, SOCKOPT_TTL,
    SOCKOPT_TYPE, SOCKOPT_ERROR, SOCKOPT_ACCEPTCONN,
};
int NetSockSetOpt(int s, int opt, UINT32 value);       /* 0 or -err */
int NetSockGetOpt(int s, int opt, UINT32 *value);      /* 0 or -err */

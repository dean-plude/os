/*
 * net.h — NovaOS networking: interface setup and asynchronous operations
 *
 * lwIP (third_party/lwip) runs on the "net" kernel thread.  Apps start
 * operations (DNS lookup, ping, HTTP GET), then poll op->state from their
 * UI tick.  Operations come from a fixed pool owned by this module: an
 * app calls NetRelease() when done (even if the operation is still
 * running); the slot is recycled only once lwIP can no longer touch it.
 */

#pragma once

#include "../include/types.h"

/* Bring up the NIC, lwIP and DHCP, and start the net thread. */
bool NetInitialize(void);
bool NetAvailable(void);

typedef struct {
    bool   present;          /* a supported NIC was found */
    bool   link;             /* cable/link up */
    bool   configured;       /* DHCP lease obtained */
    UINT8  mac[6];
    UINT32 ip, mask, gw;     /* network byte order (as lwIP stores them) */
    UINT32 dns[2];
    const char *adapter;
} NetStatus;

void NetGetStatus(NetStatus *st);
void NetFormatIp(UINT32 ip_be, char *buf, int cap);        /* "10.0.2.15" */
bool NetParseIp(const char *s, UINT32 *ip_be);

/* -----------------------------------------------------------------------
 * Asynchronous operations
 * ----------------------------------------------------------------------- */
typedef enum { NET_PENDING = 0, NET_DONE = 1, NET_FAILED = -1 } NetState;
typedef enum { NETOP_DNS, NETOP_PING, NETOP_HTTP } NetOpKind;

#define NET_HTTP_MAX  (256u * 1024 * 1024) /* largest response kept (App Store installers) */

typedef struct NetOp {
    NetOpKind       kind;
    volatile int    state;          /* NetState */
    char            error[64];      /* set when NET_FAILED */

    UINT32          ip;             /* DNS result / ping or HTTP target */

    /* ping */
    UINT16          seq;
    int             rtt_ms;
    int             ttl;

    /* HTTP: the response (status line + headers + body; a chunked body
     * is delivered already decoded) */
    char           *data;
    UINT32          len;
    char            tls_info[64];   /* https: "TLS 1.3, TLS_AES_128_GCM_SHA256" */
    bool            reused;         /* sent on a kept-alive connection */
    bool            resumed;        /* TLS session resumed (abbreviated handshake) */

    /* private */
    bool            in_use, released, retried;
    UINT64          started, deadline;   /* ticks */
    void           *conn;           /* http.c connection while in flight */
    UINT32          cap;
    char            request[768];
} NetOp;

/* Resolve a host name (or dotted-quad literal) to an IPv4 address. */
NetOp *NetResolve(const char *host);
/* Send one ICMP echo request; completes on the reply or after 2 s. */
NetOp *NetPing(UINT32 ip_be, UINT16 seq);
/* HTTP/1.1 GET http[s]://host:port/path.  Connections are kept alive and
 * reused for later requests to the same server; with @https the
 * connection uses TLS 1.2/1.3, the server's certificate must chain to a
 * trusted root and match @host, and sessions are resumed when possible. */
NetOp *NetHttpGet(UINT32 ip_be, UINT16 port, const char *host, const char *path,
                  bool https);
/* Done with an operation (safe while it is still pending). */
void   NetRelease(NetOp *op);

/* Split "http://host[:port]/path" (scheme optional).  *https is set for
 * https:// URLs (default port 443).  Returns false if malformed. */
bool   NetParseUrl(const char *url, char *host, int host_cap, UINT16 *port,
                   char *path, int path_cap, bool *https);

/* Parse a completed HTTP response. Returns the status code (0 if
 * malformed); body/body_len point into op->data; location (may be NULL)
 * receives a Location header if present. */
int    NetHttpParse(const NetOp *op, const char **body, UINT32 *body_len,
                    char *location, int loc_cap);

/* -----------------------------------------------------------------------
 * Trusted root certificates (HTTPS)
 * ----------------------------------------------------------------------- */
/* Number of trusted roots; *imported (may be NULL) gets the user-added count. */
int    NetRootCount(int *imported);
/* Name of root i (its common name); *imported set for user-added roots. */
bool   NetRootName(int i, char *buf, int cap, bool *imported);
/* Trust the CA certificate(s) in a PEM or DER file.  Returns the number
 * added; 0 with *err set on failure.  Lasts until reboot. */
int    NetImportRoots(const void *data, UINT32 len, char *err, int err_cap);

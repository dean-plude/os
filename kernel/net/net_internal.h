/*
 * net_internal.h — shared between net.c (lwIP glue, DNS, ping, the net
 * thread) and http.c (HTTP/HTTPS client).  Everything here must be called
 * with the network lock held unless noted.
 */
#pragma once
#include "net.h"

void   net_lock(void);                  /* (takes the lock) */
void   net_unlock(void);

/* Waiting for the network: read net_gen() before checking a condition,
 * then net_wait(gen) sleeps until something changes (net_wake: data,
 * connections, operations finishing) or 100 ms pass.  net_wait(gen, 0)
 * returns at once if the generation moved on. */
UINT32 net_gen(void);
void   net_wait(UINT32 gen);
void   net_wait_ticks(UINT32 gen, UINT64 max_ticks);
void   net_wake(void);
bool   net_up(void);

NetOp *net_op_alloc(NetOpKind kind);
void   net_op_fail(NetOp *op, const char *why);
void   net_op_done(NetOp *op);
void   net_op_free(NetOp *op);
/* Append response bytes; false (and the op failed) if too large or OOM. */
bool   net_op_append(NetOp *op, const void *data, UINT32 n);

/* http.c */
void   http_poll(void);                 /* net thread: timeouts, idle connections */
void   http_release(NetOp *op);         /* NetRelease() of an HTTP operation */

/* NetIp ↔ lwIP addresses (an IPv6 link-local address gets the interface's
 * zone).  The lwIP type is ip_addr_t; void * keeps lwIP out of this header. */
void   net_addr_to_lwip(const NetIp *a, void *ip_addr_out);
void   net_addr_from_lwip(const void *ip_addr, NetIp *out);

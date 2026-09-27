/*
 * net_internal.h — shared between net.c (lwIP glue, DNS, ping, the net
 * thread) and http.c (HTTP/HTTPS client).  Everything here must be called
 * with the network lock held unless noted.
 */
#pragma once
#include "net.h"

void   net_lock(void);                  /* (takes the lock) */
void   net_unlock(void);
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

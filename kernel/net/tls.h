/*
 * tls.h — NovaOS TLS: Mbed TLS glue, root certificate store, entropy
 *
 * Mbed TLS (third_party/mbedtls) provides TLS 1.2 and 1.3 with full X.509
 * chain validation against the trusted roots below.  A TlsConn wraps one
 * client session over an lwIP TCP connection: received ciphertext is
 * queued with TlsInput(); TlsHandshake/TlsRead/TlsWrite then run the
 * protocol, writing records straight to the TCP connection.  Sessions are
 * cached per host so reconnects resume without a full handshake.
 *
 * All functions must be called with the network lock held (net.c).
 */

#pragma once

#include "../include/types.h"

/* Built-in roots: Mozilla's CA list as DER, generated into tls_roots.c */
typedef struct { const UINT8 *der; UINT32 len; } TlsRootDer;
extern const TlsRootDer g_tls_builtin_roots[];
extern const int        g_tls_builtin_root_count;

/* Entropy pool, crypto library and root store (once, at network start). */
void TlsInit(void);
/* Mix an unpredictable sample (e.g. a timestamp) into the entropy pool. */
void TlsStirEntropy(const void *data, UINT32 len);
/* Fill @out with pool output (for Mbed TLS's entropy source). */
void TlsEntropyOutput(UINT8 *out, size_t len);

/* Trusted root store: built-in roots followed by user-imported ones. */
int  TlsRootCount(void);
int  TlsUserRootCount(void);
/* Display name of root i (its CN, else O/OU); *user set for imported roots. */
bool TlsRootName(int i, char *buf, int cap, bool *user);
/* Import CA certificates (PEM, one or more; or a single DER certificate)
 * as trusted roots.  Returns the number added (0 with *err set if none). */
int  TlsImportRoots(const void *data, UINT32 len, char *err, int err_cap);

typedef struct TlsConn TlsConn;

#define TLS_AGAIN   0          /* would block: wait for more TCP data/space */
#define TLS_EOF    (-0x10000)  /* TlsRead: the peer closed (outside Mbed TLS error range) */

/* New client session for @host:@port over lwIP TCP pcb @pcb (SNI and
 * certificate name check use @host).  NULL on OOM. */
TlsConn *TlsClientNew(const char *host, UINT16 port, void *pcb);
/* Queue a received pbuf (takes ownership). */
void     TlsInput(TlsConn *c, void *pbuf);
/* Advance the handshake: 1 = complete, TLS_AGAIN, or a negative error. */
int      TlsHandshake(TlsConn *c);
/* Bytes written (>0), TLS_AGAIN, or a negative error. */
int      TlsWrite(TlsConn *c, const void *data, size_t len);
/* Bytes read (>0), TLS_AGAIN, TLS_EOF, or a negative error. */
int      TlsRead(TlsConn *c, void *buf, size_t cap);
/* Remember the session so the next connection to this host resumes it. */
void     TlsSaveSession(TlsConn *c);
/* Send close_notify (best effort) and free. */
void     TlsClose(TlsConn *c);
/* Free without sending anything (connection already gone). */
void     TlsFree(TlsConn *c);

/* After a completed handshake: "TLS 1.3, TLS_AES_128_GCM_SHA256". */
const char *TlsDescribe(TlsConn *c, char *buf, int cap);
bool        TlsResumed(TlsConn *c);
/* Human-readable text for an error from TlsHandshake/TlsRead/TlsWrite. */
const char *TlsErrorText(TlsConn *c, int err, char *buf, int cap);

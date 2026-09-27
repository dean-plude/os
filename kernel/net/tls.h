/*
 * tls.h — NovaOS TLS support: BearSSL glue, root certificate store, entropy
 *
 * BearSSL (third_party/bearssl) does the protocol work: TLS 1.0-1.2 with
 * ECDHE/RSA key exchange, AES-GCM/CBC and ChaCha20-Poly1305, and full
 * X.509 chain validation against the trusted roots below.  net.c drives
 * the engine over lwIP TCP connections.
 *
 * All functions must be called with the network lock held (net.c).
 */

#pragma once

#include "../include/types.h"
#include "bearssl.h"

/* Built-in roots: Mozilla's CA list, generated into tls_roots.c */
extern const br_x509_trust_anchor g_tls_builtin_roots[];
extern const int                  g_tls_builtin_root_count;

/* Seed the entropy pool (once, at network start-up). */
void TlsInit(void);
/* Mix an unpredictable sample (e.g. a timestamp) into the entropy pool. */
void TlsStirEntropy(const void *data, UINT32 len);

/* Trusted root store: built-in roots followed by user-imported ones. */
int  TlsRootCount(void);
int  TlsUserRootCount(void);
/* Display name of root i (its CN, else O/OU); *user set for imported roots. */
bool TlsRootName(int i, char *buf, int cap, bool *user);
/* Import CA certificates (PEM, one or more; or a single DER certificate)
 * as trusted roots.  Returns the number added (0 with *err set if none). */
int  TlsImportRoots(const void *data, UINT32 len, char *err, int err_cap);

/* One client connection: engine, X.509 validator and record buffers. */
typedef struct TlsConn {
    br_ssl_client_context   cc;
    br_x509_minimal_context xc;
    unsigned char           iobuf[BR_SSL_BUFSIZE_BIDI];
} TlsConn;

/* Create a client for @host (SNI + certificate name check).  NULL on OOM
 * or bad parameters. */
TlsConn *TlsClientNew(const char *host);
void     TlsFree(TlsConn *c);

/* Human-readable text for a BearSSL error code. */
const char *TlsErrorText(int err, char *buf, int cap);

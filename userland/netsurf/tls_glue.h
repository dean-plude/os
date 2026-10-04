/* tls_glue.h — Mbed TLS set-up for NetSurf on NovaOS (tls_glue.c) */
#pragma once
#include "mbedtls/ssl.h"

/* Seed the DRBG, load the roots from @roots_path and build the client
 * configurations.  0 on success; safe to call again. */
int nova_tls_init(const char *roots_path);
int nova_tls_root_count(void);
/* The trusted roots themselves (crypt32's ROOT store and chain engine) */
const mbedtls_x509_crt *nova_tls_roots(void);
/* Shared, read-only client configuration: TLS 1.2-1.3, or 1.2 only */
const mbedtls_ssl_config *nova_tls_config(int tls12_only);
/* A fresh client configuration like nova_tls_config's, for a caller to
 * adjust (ALPN, verification) and free; after nova_tls_init.  0 on success. */
int nova_tls_client_config(mbedtls_ssl_config *c, int tls12_only);

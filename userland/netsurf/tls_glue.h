/* tls_glue.h — Mbed TLS set-up for NetSurf on NovaOS (tls_glue.c) */
#pragma once
#include "mbedtls/ssl.h"

/* Seed the DRBG, load the roots from @roots_path and build the client
 * configurations.  0 on success; safe to call again. */
int nova_tls_init(const char *roots_path);
int nova_tls_root_count(void);
/* Shared, read-only client configuration: TLS 1.2-1.3, or 1.2 only */
const mbedtls_ssl_config *nova_tls_config(int tls12_only);

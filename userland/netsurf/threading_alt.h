/* threading_alt.h — Mbed TLS mutexes for NovaOS programs (see tls_glue.c) */
#pragma once

typedef struct mbedtls_threading_mutex_t {
    void *cs;           /* CRITICAL_SECTION, allocated by the init hook */
    char  is_valid;
} mbedtls_threading_mutex_t;

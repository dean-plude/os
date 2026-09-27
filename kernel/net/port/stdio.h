/* stdio.h shim for Mbed TLS: file I/O is compiled out (no MBEDTLS_FS_IO) */
#pragma once
#include <stddef.h>
typedef struct nova_FILE FILE;

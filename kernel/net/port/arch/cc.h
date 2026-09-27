/*
 * arch/cc.h — lwIP platform definitions for the NovaOS kernel
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../../../include/types.h"

/* The kernel is freestanding: no <inttypes.h>, <ctype.h>, <unistd.h>. */
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_CTYPE_H    1
#define LWIP_NO_UNISTD_H   1
typedef long ssize_t;
#define SSIZE_MAX 0x7FFFFFFFFFFFFFFFL

#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "zu"

#define BYTE_ORDER LITTLE_ENDIAN

void kprintf(const char *fmt, ...);
void net_assert_fail(const char *msg, const char *file, int line) __attribute__((noreturn));
unsigned int net_random(void);

#define LWIP_PLATFORM_DIAG(x)   do { kprintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) net_assert_fail((x), __FILE__, __LINE__)
#define LWIP_RAND()             ((u32_t)net_random())

#define LWIP_ERR_T int

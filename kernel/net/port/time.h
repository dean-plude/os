/* time.h shim for Mbed TLS: only struct tm is used (gmtime_r is provided
 * by net/tls_platform.c via MBEDTLS_PLATFORM_GMTIME_R_ALT) */
#pragma once
typedef long long time_t;
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

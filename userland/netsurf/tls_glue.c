/*
 * tls_glue.c — Mbed TLS in a NovaOS program: entropy, time, mutexes and
 * the trusted roots, plus the shared client configuration the NetSurf
 * fetcher (fetch_nova.c) uses.
 *
 * Entropy comes from the kernel's pool (NtNovaGetRandom), wall-clock time
 * from msvcrt, mutexes are Win32 critical sections, and the roots are the
 * Mozilla bundle the build writes next to NetSurf's resources
 * (res/ca-bundle.der: DER certificates back to back) plus any the user
 * added with certutil (C:\Windows\System32\CertStore).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <winternl.h>

#include "mbedtls/platform_time.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/threading.h"
#include "psa/crypto.h"

#include "tls_glue.h"

/* -----------------------------------------------------------------------
 * Platform hooks named by the configuration
 * ----------------------------------------------------------------------- */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    size_t done = 0;
    while (done < len) {
        ULONG n = (ULONG)(len - done > 4096 ? 4096 : len - done);
        if (!NT_SUCCESS(NtNovaGetRandom(output + done, n))) break;
        done += n;
    }
    *olen = done;
    return done ? 0 : MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)GetTickCount64();
}

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf)
{
    /* computed here: msvcrt's gmtime has one static result, and fetches
     * verify certificates on several threads at once */
    long long t = (long long)*tt;
    long long days = t / 86400, secs = t % 86400;
    if (secs < 0) { secs += 86400; days--; }
    memset(tm_buf, 0, sizeof(*tm_buf));
    tm_buf->tm_hour = (int)(secs / 3600);
    tm_buf->tm_min = (int)(secs / 60 % 60);
    tm_buf->tm_sec = (int)(secs % 60);
    tm_buf->tm_wday = (int)((days % 7 + 11) % 7);          /* 1970-01-01 was a Thursday */
    /* civil_from_days (H. Hinnant) */
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    long long d = doy - (153 * mp + 2) / 5 + 1;
    long long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    tm_buf->tm_year = (int)(y - 1900);
    tm_buf->tm_mon = (int)(m - 1);
    tm_buf->tm_mday = (int)d;
    return tm_buf;
}

/* -----------------------------------------------------------------------
 * Mutexes
 * ----------------------------------------------------------------------- */
static void mutex_init(mbedtls_threading_mutex_t *m)
{
    CRITICAL_SECTION *cs = malloc(sizeof(*cs));
    m->cs = cs;
    m->is_valid = cs != NULL;
    if (cs) InitializeCriticalSection(cs);
}

static void mutex_free(mbedtls_threading_mutex_t *m)
{
    if (!m->is_valid) return;
    DeleteCriticalSection(m->cs);
    free(m->cs);
    m->cs = NULL;
    m->is_valid = 0;
}

static int mutex_lock(mbedtls_threading_mutex_t *m)
{
    if (!m || !m->is_valid) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    EnterCriticalSection(m->cs);
    return 0;
}

static int mutex_unlock(mbedtls_threading_mutex_t *m)
{
    if (!m || !m->is_valid) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    LeaveCriticalSection(m->cs);
    return 0;
}

/* -----------------------------------------------------------------------
 * Client configuration
 * ----------------------------------------------------------------------- */
static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt         g_roots;
static mbedtls_ssl_config       g_conf, g_conf12;
static int                      g_ready, g_nroots;

/* The bundle is DER certificates back to back; each starts with a
 * SEQUENCE header giving its length. */
static void load_roots(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        size_t off = 0;
        while (off + 4 <= (size_t)n && buf[off] == 0x30) {
            size_t len, hdr;
            unsigned char l = buf[off + 1];
            if (l < 0x80) { len = l; hdr = 2; }
            else if (l == 0x81) { len = buf[off + 2]; hdr = 3; }
            else if (l == 0x82) { len = (size_t)buf[off + 2] << 8 | buf[off + 3]; hdr = 4; }
            else break;
            if (off + hdr + len > (size_t)n) break;
            if (mbedtls_x509_crt_parse_der(&g_roots, buf + off, hdr + len) == 0) g_nroots++;
            off += hdr + len;
        }
    }
    free(buf);
    fclose(f);
}

/* Roots the user trusted with "certutil -addstore root" (the Terminal
 * keeps a copy of each in C:\Windows\System32\CertStore): PEM or DER */
static void load_store(const char *dir)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *buf = n > 0 ? malloc((size_t)n + 1) : NULL;
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            buf[n] = 0;                 /* PEM parsing wants the terminator counted */
            int before = g_nroots;
            if (mbedtls_x509_crt_parse(&g_roots, buf, (size_t)n + 1) >= 0 ||
                mbedtls_x509_crt_parse_der(&g_roots, buf, (size_t)n) == 0)
                g_nroots = before + 1;
        }
        free(buf);
        fclose(f);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int conf_setup(mbedtls_ssl_config *c, int tls12_only)
{
    mbedtls_ssl_config_init(c);
    if (mbedtls_ssl_config_defaults(c, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT))
        return -1;
    mbedtls_ssl_conf_authmode(c, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(c, &g_roots, NULL);
    mbedtls_ssl_conf_rng(c, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_min_tls_version(c, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(c, tls12_only ? MBEDTLS_SSL_VERSION_TLS1_2 : MBEDTLS_SSL_VERSION_TLS1_3);
    static const char *alpn[] = { "http/1.1", NULL };
    mbedtls_ssl_conf_alpn_protocols(c, alpn);
    return 0;
}

int nova_tls_init(const char *roots_path)
{
    if (g_ready) return 0;
    mbedtls_threading_set_alt(mutex_init, mutex_free, mutex_lock, mutex_unlock);
    if (psa_crypto_init() != PSA_SUCCESS) return -1;
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    static const char pers[] = "NovaOS NetSurf";
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                              (const unsigned char *)pers, sizeof(pers) - 1))
        return -1;
    mbedtls_x509_crt_init(&g_roots);
    load_roots(roots_path);
    load_store("C:\\Windows\\System32\\CertStore");
    if (conf_setup(&g_conf, 0) || conf_setup(&g_conf12, 1)) return -1;
    g_ready = 1;
    return 0;
}

int nova_tls_root_count(void) { return g_nroots; }

const mbedtls_ssl_config *nova_tls_config(int tls12_only)
{
    return tls12_only ? &g_conf12 : &g_conf;
}

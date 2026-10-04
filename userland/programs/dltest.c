/* dltest.exe — a long download over several connections at once, as game
 * stores and installers fetch their packages (usage: dltest [-n conns]
 * [-t stall-seconds] [-w] host port bytes)
 *
 * Opens @conns non-blocking sockets to tools/h2server.js's HTTP port,
 * asks each for /stream/BYTES/SEED (BYTES of a pattern seeded by SEED) and
 * reads them all through select(), as libcurl's multi interface does.
 * Every byte is checked against the pattern.  With -w it also does what an
 * installer does meanwhile: writes each download to a file in
 * C:\Temp\dltest and, on two more threads, maps files into memory over
 * and over (NtCreateSection takes the desktop lock, which the desktop
 * takes and holds while it reads the network's state for the dock).  It
 * fails when the transfer as a whole makes no progress for @stall seconds
 * (default 5), when a connection ends early, or when a byte is wrong;
 * otherwise it prints the throughput and the longest pause. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#define MAXC 16

typedef struct {
    SOCKET   s;
    int      connected, header_done, done;
    char     head[1024];
    int      head_len;
    unsigned long long got;       /* body bytes */
    unsigned seed;
} Conn;

/* Byte i of stream @seed (tools/h2server.js computes the same) */
static unsigned char pattern(unsigned seed, unsigned long long i)
{
    return (unsigned char)((i * 7 + seed * 13 + (i >> 16)) % 251);
}

static volatile LONG g_stop;
static volatile unsigned g_sink;           /* (what the mappers read, so it is read) */
static const char g_dir[] = "C:\\Temp\\dltest";

/* -w: map a file and touch it, over and over, until the download ends */
static DWORD WINAPI mapper(void *arg)
{
    char path[64];
    snprintf(path, sizeof(path), "%s\\map%d.bin", g_dir, (int)(INT_PTR)arg);
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, CREATE_ALWAYS, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return 1;
    static char block[65536];
    DWORD wrote;
    for (int i = 0; i < 4; i++) WriteFile(f, block, sizeof(block), &wrote, 0);
    unsigned maps = 0;
    while (!g_stop) {
        HANDLE m = CreateFileMappingW(f, 0, PAGE_READONLY, 0, 0, 0);
        if (m) {
            const unsigned char *v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
            if (v) { g_sink += v[maps % 4096]; UnmapViewOfFile(v); }
            CloseHandle(m);
        }
        maps++;
    }
    CloseHandle(f);
    DeleteFileA(path);
    return 0;
}

static int in_set(SOCKET s, const fd_set *f)
{
    for (u_int i = 0; i < f->fd_count; i++) if (f->fd_array[i] == s) return 1;
    return 0;
}

static int fail(const char *why, int c)
{
    if (c >= 0) printf("dltest: FAIL: connection %d: %s\n", c, why);
    else printf("dltest: FAIL: %s\n", why);
    return 1;
}

int main(int argc, char **argv)
{
    int n = 4, stall_limit = 5, files = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) stall_limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w")) files = 1;
    }
    if (argc - i < 3 || n < 1 || n > MAXC) {
        printf("usage: dltest [-n connections (1-%d)] [-t stall-seconds] [-w] host port bytes\n", MAXC);
        return 1;
    }
    const char *host = argv[i], *port = argv[i + 1];
    unsigned long long size = _strtoui64(argv[i + 2], 0, 10);

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) return fail("WSAStartup", -1);
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res)) return fail("cannot resolve the host", -1);

    static Conn c[MAXC];
    HANDLE out[MAXC] = { 0 }, maps[2] = { 0 };
    if (files) {
        CreateDirectoryA("C:\\Temp", 0);
        CreateDirectoryA(g_dir, 0);
        for (int k = 0; k < n; k++) {
            char path[64];
            snprintf(path, sizeof(path), "%s\\%d.bin", g_dir, k);
            out[k] = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, 0, 0);
            if (out[k] == INVALID_HANDLE_VALUE) return fail("cannot create the file", k);
        }
        for (int k = 0; k < 2; k++) maps[k] = CreateThread(0, 0, mapper, (void *)(INT_PTR)k, 0, 0);
    }
    for (int k = 0; k < n; k++) {
        c[k].seed = (unsigned)k + 1;
        c[k].s = socket(AF_INET, SOCK_STREAM, 0);
        u_long nb = 1;
        ioctlsocket(c[k].s, FIONBIO, &nb);
        if (connect(c[k].s, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR &&
            WSAGetLastError() != WSAEWOULDBLOCK)
            return fail("connect failed", k);
    }
    freeaddrinfo(res);

    static char buf[65536];
    DWORD start = GetTickCount(), last_progress = start, last_report = start, longest = 0;
    unsigned long long total = 0, total_at_report = 0;
    int left = n;
    printf("dltest: %d connections, %llu bytes each from %s:%s\n", n, size, host, port);
    while (left) {
        fd_set rd, wr;
        FD_ZERO(&rd); FD_ZERO(&wr);
        for (int k = 0; k < n; k++) {
            if (c[k].done) continue;
            if (c[k].connected) FD_SET(c[k].s, &rd); else FD_SET(c[k].s, &wr);
        }
        struct timeval tv = { 0, 250000 };
        int r = select(0, &rd, &wr, 0, &tv);
        if (r == SOCKET_ERROR) return fail("select failed", -1);
        for (int k = 0; k < n; k++) {
            Conn *x = &c[k];
            if (x->done) continue;
            if (!x->connected && in_set(x->s, &wr)) {
                char req[128];
                int len = snprintf(req, sizeof(req), "GET /stream/%llu/%u HTTP/1.0\r\nHost: %s\r\n\r\n",
                                   size, x->seed, host);
                if (send(x->s, req, len, 0) != len) return fail("send failed", k);
                x->connected = 1;
                continue;
            }
            if (!x->connected || !in_set(x->s, &rd)) continue;
            for (;;) {                                      /* drain what is there */
                int got = recv(x->s, buf, sizeof(buf), 0);
                if (got == SOCKET_ERROR) {
                    if (WSAGetLastError() == WSAEWOULDBLOCK) break;
                    return fail("recv failed", k);
                }
                if (got == 0) {
                    if (x->got != size) {
                        printf("dltest: connection %d ended after %llu of %llu bytes\n", k, x->got, size);
                        return fail("ended early", k);
                    }
                    x->done = 1; left--;
                    closesocket(x->s);
                    break;
                }
                int off = 0;
                while (!x->header_done && off < got) {      /* the response header, up to the blank line */
                    if (x->head_len < (int)sizeof(x->head) - 1) x->head[x->head_len++] = buf[off];
                    off++;
                    if (x->head_len >= 4 && !memcmp(x->head + x->head_len - 4, "\r\n\r\n", 4)) {
                        x->header_done = 1;
                        x->head[x->head_len] = 0;
                        if (strncmp(x->head, "HTTP/1.1 200", 12) && strncmp(x->head, "HTTP/1.0 200", 12))
                            return fail("not 200 OK", k);
                    }
                }
                for (; off < got; off++, x->got++)
                    if ((unsigned char)buf[off] != pattern(x->seed, x->got)) {
                        printf("dltest: connection %d: byte %llu is %u, not %u\n", k, x->got,
                               (unsigned char)buf[off], pattern(x->seed, x->got));
                        return fail("wrong data", k);
                    }
                total += (unsigned)got;
                last_progress = GetTickCount();
                DWORD wrote;
                if (out[k] && (!WriteFile(out[k], buf, (DWORD)got, &wrote, 0) || wrote != (DWORD)got))
                    return fail("cannot write the file", k);
            }
        }
        DWORD now = GetTickCount();
        if (now - last_progress > longest) longest = now - last_progress;
        if (now - last_progress > (DWORD)stall_limit * 1000) {
            printf("dltest: no data for %d s at %llu of %llu bytes\n", stall_limit, total, size * n);
            for (int k = 0; k < n; k++)
                printf("dltest: connection %d: %llu bytes%s\n", k, c[k].got, c[k].done ? ", finished" : "");
            return fail("the download stalled", -1);
        }
        if (now - last_report >= 5000) {                   /* progress every 5 s */
            printf("dltest: %llu MB, %llu KB/s\n", total >> 20,
                   (total - total_at_report) * 1000 / 1024 / (now - last_report));
            last_report = now;
            total_at_report = total;
        }
    }
    g_stop = 1;
    if (files) {
        WaitForMultipleObjects(2, maps, TRUE, 10000);
        for (int k = 0; k < n; k++) {
            char path[64];
            snprintf(path, sizeof(path), "%s\\%d.bin", g_dir, k);
            CloseHandle(out[k]);
            DeleteFileA(path);
        }
        RemoveDirectoryA(g_dir);
    }
    DWORD ms = GetTickCount() - start;
    if (!ms) ms = 1;
    printf("dltest: %llu bytes in %lu.%02lu s, %llu KB/s, longest pause %lu ms\n", total, ms / 1000,
           ms % 1000 / 10, total * 1000 / 1024 / ms, longest);
    printf("dltest: PASS\n");
    return 0;
}

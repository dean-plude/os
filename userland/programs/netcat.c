/* netcat.exe — a tiny HTTP/1.0 client over Winsock (usage: netcat [-4|-6] [-p port] host [path])
 *
 * Resolves with getaddrinfo and tries each address in turn, as Windows
 * programs do, so it works over IPv4 and IPv6 alike. */
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

int main(int argc, char **argv)
{
    int family = AF_UNSPEC, i = 1;
    const char *port = "http";
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-4")) family = AF_INET;
        else if (!strcmp(argv[i], "-6")) family = AF_INET6;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = argv[++i];
        else break;
    }
    if (i >= argc) { printf("usage: netcat [-4|-6] [-p port] <host> [path]\n"); return 1; }
    const char *host = argv[i];
    const char *path = i + 1 < argc ? argv[i + 1] : "/";

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { printf("WSAStartup failed\n"); return 1; }
    printf("Winsock: %s\n", w.szDescription);

    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) { printf("cannot resolve %s (err %d)\n", host, rc); return 1; }

    SOCKET s = INVALID_SOCKET;
    for (ai = res; ai; ai = ai->ai_next) {
        char text[INET6_ADDRSTRLEN];
        void *a = ai->ai_family == AF_INET6 ? (void *)&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr
                                            : (void *)&((struct sockaddr_in *)ai->ai_addr)->sin_addr;
        inet_ntop(ai->ai_family, a, text, sizeof(text));
        printf("%s -> %s (%s)\n", host, text, ai->ai_family == AF_INET6 ? "IPv6" : "IPv4");
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) { printf("socket failed (err %d)\n", WSAGetLastError()); continue; }
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        printf("connect failed (err %d)\n", WSAGetLastError());
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) return 1;

    struct sockaddr_storage me, peer;
    int ml = sizeof(me), pl = sizeof(peer);
    char ms[64], ps[64];
    DWORD msl = sizeof(ms), psl = sizeof(ps);
    if (getsockname(s, (struct sockaddr *)&me, &ml) == 0 && getpeername(s, (struct sockaddr *)&peer, &pl) == 0 &&
        WSAAddressToStringA((struct sockaddr *)&me, ml, 0, ms, &msl) == 0 &&
        WSAAddressToStringA((struct sockaddr *)&peer, pl, 0, ps, &psl) == 0)
        printf("connected %s -> %s\n", ms, ps);
    else
        printf("connected\n");

    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    if (send(s, req, n, 0) != n) { printf("send failed\n"); closesocket(s); return 1; }

    char buf[2048];
    int total = 0, got, shown = 0;
    while ((got = recv(s, buf, sizeof(buf) - 1, 0)) > 0) {
        total += got;
        if (shown < 400) {                          /* print the start of the response */
            int k = got; if (shown + k > 400) k = 400 - shown;
            buf[k] = 0;
            fputs(buf, stdout);
            shown += k;
        }
    }
    printf("\n[received %d bytes total]\n", total);
    closesocket(s);
    WSACleanup();
    return 0;
}

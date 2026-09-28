/* netcat.exe — a tiny HTTP/1.0 client over Winsock (usage: netcat host [path]) */
#include <stdio.h>
#include <string.h>
#include <winsock2.h>

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: netcat <host> [path]\n"); return 1; }
    const char *host = argv[1];
    const char *path = argc > 2 ? argv[2] : "/";

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { printf("WSAStartup failed\n"); return 1; }
    printf("Winsock: %s\n", w.szDescription);

    struct hostent *he = gethostbyname(host);
    if (!he) { printf("cannot resolve %s (err %d)\n", host, WSAGetLastError()); return 1; }
    struct in_addr a; a.s_addr = *(unsigned long *)he->h_addr;
    printf("%s -> %s\n", host, inet_ntoa(a));

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { printf("socket failed\n"); return 1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    sa.sin_addr.s_addr = a.s_addr;
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        printf("connect failed (err %d)\n", WSAGetLastError());
        closesocket(s); return 1;
    }
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

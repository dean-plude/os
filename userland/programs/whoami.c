/*
 * whoami.exe — the user this program runs as
 *
 *   whoami          nova-pc\name, in lower case as Windows prints it
 *   whoami /user    the same with the user's SID
 *
 * The name is the one given in the first-boot setup (GetUserName).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    char user[64], pc[32], line[160];
    DWORD n = sizeof(user), m = sizeof(pc);
    if (!GetUserNameA(user, &n)) { fprintf(stderr, "ERROR: GetUserName failed (%lu).\n", GetLastError()); return 1; }
    if (!GetComputerNameA(pc, &m)) strcpy(pc, "nova-pc");
    _snprintf(line, sizeof(line), "%s\\%s", pc, user);
    line[sizeof(line) - 1] = '\0';
    CharLowerA(line);
    if (argc > 1 && !_stricmp(argv[1], "/user")) {
        HANDLE tok;
        BYTE buf[256];
        DWORD len;
        char *sid = NULL;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok) &&
            GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len))
            ConvertSidToStringSidA(((TOKEN_USER *)buf)->User.Sid, &sid);
        printf("\nUSER INFORMATION\n----------------\n\n%-24s %s\n%-24s %s\n", "User Name", "SID", line, sid ? sid : "");
        if (sid) LocalFree(sid);
        return 0;
    }
    printf("%s\n", line);
    return 0;
}

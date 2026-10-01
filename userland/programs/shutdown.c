/*
 * shutdown.exe — shut down or restart the computer
 *
 *   shutdown /s | /p      shut down (power off)
 *   shutdown /r           restart
 *   shutdown /t N /f /c "comment" /d ...   accepted; NovaOS acts at once
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void usage(void)
{
    printf("Usage: shutdown [/s | /p | /r] [/f] [/t xxx] [/c \"comment\"]\n\n"
           "    /s    Shut down the computer.\n"
           "    /p    Turn off the computer at once.\n"
           "    /r    Restart the computer.\n"
           "    /f    Close running programs without warning.\n"
           "    /t    Time-out before shutting down (NovaOS doesn't wait).\n");
}

int main(int argc, char **argv)
{
    UINT what = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '/' && a[0] != '-') { usage(); return 1; }
        switch (a[1] | 0x20) {
        case 's': case 'p': what = EWX_SHUTDOWN | EWX_POWEROFF; break;
        case 'r':           what = EWX_REBOOT; break;
        case 'f':           break;
        case 't': case 'c': case 'd': if (i + 1 < argc) i++; break;
        case 'a':
            fprintf(stderr, "Unable to abort the system shutdown because no shutdown was in progress.(1116)\n");
            return 1116;
        case '?': default:  usage(); return a[1] == '?' ? 0 : 1;
        }
    }
    if (!what) { usage(); return 0; }
    if (!ExitWindowsEx(what | EWX_FORCE, 0)) {
        fprintf(stderr, "shutdown: failed (error %lu)\n", GetLastError());
        return 1;
    }
    return 0;
}

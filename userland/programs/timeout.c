/*
 * timeout.exe — wait
 *
 *   timeout /t seconds [/nobreak]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    int secs = -2, quiet = 0;
    for (int i = 1; i < argc; i++) {
        if ((!_stricmp(argv[i], "/t") || !_stricmp(argv[i], "-t")) && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!_stricmp(argv[i], "/nobreak")) quiet = quiet;
        else if (argv[i][0] != '/') secs = atoi(argv[i]);
    }
    if (secs < -1) { fprintf(stderr, "ERROR: Invalid syntax. Default option is not allowed more than '1' time(s).\n"); return 1; }
    if (secs < 0) secs = 0;
    for (int left = secs; left > 0; left--) {
        printf("\rWaiting for %2d seconds, press CTRL+C to quit ...", left);
        fflush(stdout);
        Sleep(1000);
    }
    printf("\rWaiting for  0 seconds, press CTRL+C to quit ...\n");
    return 0;
}

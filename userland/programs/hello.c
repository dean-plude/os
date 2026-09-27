/* hello.exe — the first Windows program NovaOS runs */
#include <stdio.h>
#include <windows.h>

int main(int argc, char **argv)
{
    char dir[MAX_PATH];
    OSVERSIONINFOA v;
    GetVersionExA(&v);
    GetCurrentDirectoryA(sizeof(dir), dir);
    printf("Hello from a real Windows program!\n");
    printf("  Running on Windows %lu.%lu (build %lu) - NovaOS, process %lu\n",
           v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, GetCurrentProcessId());
    printf("  Current directory: %s\n", dir);
    printf("  Arguments (%d):", argc);
    for (int i = 0; i < argc; i++) printf(" [%s]", argv[i]);
    printf("\n");
    return 0;
}

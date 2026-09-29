/* ftprobe.exe PATTERN — prints what FindFirstFile reports (names, sizes, times) */
#include <stdio.h>
#include <windows.h>

int main(int argc, char **argv)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(argc > 1 ? argv[1] : "*", &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("FindFirstFile failed (%lu)\n", GetLastError()); return 1; }
    do {
        SYSTEMTIME st;
        FileTimeToSystemTime(&fd.ftLastWriteTime, &st);
        printf("%-20s attr=%08lx size=%lu write=%08lx%08lx (%04d-%02d-%02d %02d:%02d) create=%08lx%08lx\n", fd.cFileName,
               fd.dwFileAttributes, fd.nFileSizeLow, fd.ftLastWriteTime.dwHighDateTime, fd.ftLastWriteTime.dwLowDateTime,
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, fd.ftCreationTime.dwHighDateTime, fd.ftCreationTime.dwLowDateTime);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

/* Classic console cursor APIs: shared state, bounds and handle validation.
 * Run on a console; x64 and x86 use the same kernel wire format. */
#include <windows.h>
#include <stdio.h>
typedef struct { DWORD size; BOOL visible; } CursorInfo;
BOOL WINAPI GetConsoleCursorInfo(HANDLE h, PVOID info);
BOOL WINAPI SetConsoleCursorInfo(HANDLE h, const void *info);
BOOL WINAPI SetConsoleCursorPosition(HANDLE h, COORD at);
static int passed, failed;
#define CHECK(name, ok) do { if (ok) passed++; else { failed++; printf("FAIL: %s\n", name); } } while (0)
int main(void)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), dup = NULL;
    CursorInfo original, got, style = {100, FALSE};
    CONSOLE_SCREEN_BUFFER_INFO screen;
    if (!GetConsoleCursorInfo(out, &original) || !GetConsoleScreenBufferInfo(out, &screen)) return 1;
    CHECK("duplicate output", DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &dup,
                                               0, FALSE, DUPLICATE_SAME_ACCESS));
    CHECK("hide block cursor", SetConsoleCursorInfo(out, &style));
    CHECK("state shared by handles", GetConsoleCursorInfo(dup, &got) && got.size == 100 && !got.visible);
    style.size = 1; style.visible = TRUE;
    CHECK("show thin cursor", SetConsoleCursorInfo(dup, &style));
    CHECK("read thin cursor", GetConsoleCursorInfo(out, &got) && got.size == 1 && got.visible);
    style.size = 0;
    CHECK("reject size zero", !SetConsoleCursorInfo(out, &style) && GetLastError() == ERROR_INVALID_PARAMETER);
    style.size = 101;
    CHECK("reject size over 100", !SetConsoleCursorInfo(out, &style) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK("failure preserves state", GetConsoleCursorInfo(out, &got) && got.size == 1 && got.visible);
    CHECK("null get info", !GetConsoleCursorInfo(out, NULL) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK("null set info", !SetConsoleCursorInfo(out, NULL) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK("invalid output handle", !SetConsoleCursorPosition(INVALID_HANDLE_VALUE, (COORD){0, 0}) &&
                                    GetLastError() == ERROR_INVALID_HANDLE);
    CHECK("input is not a screen", !GetConsoleCursorInfo(GetStdHandle(STD_INPUT_HANDLE), &got) &&
                                   GetLastError() == ERROR_INVALID_HANDLE);
    CHECK("negative column", !SetConsoleCursorPosition(out, (COORD){-1, 0}) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK("negative row", !SetConsoleCursorPosition(out, (COORD){0, -1}) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK("column at width", !SetConsoleCursorPosition(out, (COORD){screen.dwSize.X, 0}) &&
                              GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK("row at height", !SetConsoleCursorPosition(out, (COORD){0, screen.dwSize.Y}) &&
                            GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK("bottom right", SetConsoleCursorPosition(out, (COORD){screen.dwSize.X - 1, screen.dwSize.Y - 1}));
    CHECK("restore cursor style", SetConsoleCursorInfo(out, &original));
    CHECK("home cursor", SetConsoleCursorPosition(out, (COORD){0, 0}));
    if (dup) CloseHandle(dup);
    printf("consolecursortest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

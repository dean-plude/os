/* Classic console cursor APIs: shared state, bounds and handle validation.
 * Run on a console; x64 and x86 use the same kernel wire format. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
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
    WORD original_attribute = screen.wAttributes;
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
    CHECK("cursor position reported", GetConsoleScreenBufferInfo(out, &screen) &&
          screen.dwCursorPosition.X == screen.dwSize.X - 1 && screen.dwCursorPosition.Y == screen.dwSize.Y - 1);
    CHECK("restore cursor style", SetConsoleCursorInfo(out, &original));
    CHECK("home cursor", SetConsoleCursorPosition(out, (COORD){0, 0}));
    CHECK("set text attributes", SetConsoleTextAttribute(out, 0x1E) &&
          GetConsoleScreenBufferInfo(out, &screen) && screen.wAttributes == 0x1E);
    CHECK("restore text attributes", SetConsoleTextAttribute(out, original_attribute));
    DWORD done = 0;
    WCHAR text[4] = {0};
    WORD attrs[4] = {0};
    CHECK("fill characters", FillConsoleOutputCharacterW(out, L'Z', 4, (COORD){5, 6}, &done) && done == 4);
    CHECK("read filled characters", ReadConsoleOutputCharacterW(out, text, 4, (COORD){5, 6}, &done) &&
          done == 4 && text[0] == L'Z' && text[3] == L'Z');
    CHECK("fill attributes", FillConsoleOutputAttribute(out, 0x1E, 4, (COORD){5, 6}, &done) && done == 4);
    CHECK("read filled attributes", ReadConsoleOutputAttribute(out, attrs, 4, (COORD){5, 6}, &done) &&
          done == 4 && attrs[0] == 0x1E && attrs[3] == 0x1E);
    CHAR_INFO cells[4] = {0}, readback[4] = {0};
    cells[0].Char.UnicodeChar = L'A'; cells[0].Attributes = 0x1F;
    cells[1].Char.UnicodeChar = L'B'; cells[1].Attributes = 0x2E;
    cells[2].Char.UnicodeChar = L'C'; cells[2].Attributes = 0x3D;
    cells[3].Char.UnicodeChar = L'D'; cells[3].Attributes = 0x4C;
    SMALL_RECT region = {10, 10, 11, 11};
    CHECK("write cells", WriteConsoleOutputW(out, cells, (COORD){2, 2}, (COORD){0, 0}, &region));
    region = (SMALL_RECT){10, 10, 11, 11};
    CHECK("read cells", ReadConsoleOutputW(out, readback, (COORD){2, 2}, (COORD){0, 0}, &region) &&
          readback[0].Char.UnicodeChar == L'A' && readback[3].Char.UnicodeChar == L'D' &&
          readback[1].Attributes == 0x2E);
    CHAR_INFO fill = {0}; fill.Char.UnicodeChar = L'.'; fill.Attributes = 7;
    CHECK("scroll cells", ScrollConsoleScreenBufferW(out, &region, NULL, (COORD){11, 11}, &fill));
    region = (SMALL_RECT){10, 10, 10, 10};
    memset(readback, 0, sizeof(readback));
    CHECK("scroll fill verified", ReadConsoleOutputW(out, readback, (COORD){1, 1}, (COORD){0, 0}, &region) &&
          readback[0].Char.UnicodeChar == L'.');
    HANDLE separate = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, 0, NULL, 1, NULL);
    CHECK("create independent screen", separate != INVALID_HANDLE_VALUE && separate != out);
    if (separate != INVALID_HANDLE_VALUE) {
        CHECK("resize independent screen", SetConsoleScreenBufferSize(separate, (COORD){2, 2}) &&
              GetConsoleScreenBufferInfo(separate, &screen) && screen.dwSize.X == 2 && screen.dwSize.Y == 2);
        region = (SMALL_RECT){0, 0, 0, 0};
        cells[0].Char.UnicodeChar = L'X';
        CHECK("write independent screen", WriteConsoleOutputW(separate, cells, (COORD){1, 1}, (COORD){0, 0}, &region));
        memset(readback, 0, sizeof(readback));
        region = (SMALL_RECT){0, 0, 0, 0};
        CHECK("read independent screen", ReadConsoleOutputW(separate, readback, (COORD){1, 1}, (COORD){0, 0}, &region) &&
              readback[0].Char.UnicodeChar == L'X');
        CHECK("activate independent screen", SetConsoleActiveScreenBuffer(separate));
        CHECK("restore active screen", SetConsoleActiveScreenBuffer(out));
        CloseHandle(separate);
    }
    if (dup) CloseHandle(dup);
    printf("consolecursortest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

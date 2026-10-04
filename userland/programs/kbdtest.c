/* kbdtest.exe — keyboard layouts as programs see them.  Prints
 * "[KBD] layout KLID hkl HKL" for the layout in effect, then checks
 * layouts by handle whatever the user chose (US, French AZERTY, German:
 * ToUnicodeEx, MapVirtualKeyEx, VkKeyScanEx, LoadKeyboardLayout) and the
 * registry's list.  "kbdtest 00000407" also checks that German is the
 * layout in effect: GetKeyboardLayout and its name, the Y and Z keys
 * swapped, AltGr+Q typing @, the dead acute accent on e, the key names,
 * SPI_GETDEFAULTINPUTLANG and HKCU\Keyboard Layout\Preload; "kbdtest
 * 00000407 us" then makes US the user's layout again with
 * SPI_SETDEFAULTINPUTLANG and waits for the system to follow (the desktop
 * types with it from then on).  Ends with "kbdtest: N passed, M failed". */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#define HKL_US ((HKL)(ULONG_PTR)0x04090409)
#define HKL_FR ((HKL)(ULONG_PTR)0x040C040C)
#define HKL_DE ((HKL)(ULONG_PTR)0x04070407)

/* What key @vk types with the modifiers in @mods (1 Shift, 2 Ctrl, 4 Alt) in @hkl: the count ToUnicodeEx returns */
static int type(HKL hkl, UINT vk, int mods, WCHAR *out)
{
    BYTE keys[256];
    memset(keys, 0, sizeof(keys));
    if (mods & 1) keys[VK_SHIFT] = 0x80;
    if (mods & 2) keys[VK_CONTROL] = 0x80;
    if (mods & 4) keys[VK_MENU] = 0x80;
    memset(out, 0, 4 * sizeof(WCHAR));
    return ToUnicodeEx(vk, MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl), keys, out, 4, 0, hkl);
}

static void by_handle(void)
{
    WCHAR c[4];
    /* US: the keys where they are printed */
    CHECK("US: Y key types y", type(HKL_US, 'Y', 0, c) == 1 && c[0] == 'y');
    CHECK("US: Shift+2 types @", type(HKL_US, '2', 1, c) == 1 && c[0] == '@');
    CHECK("US: scan code 0x15 is VK_Y", MapVirtualKeyExW(0x15, MAPVK_VSC_TO_VK, HKL_US) == 'Y');
    CHECK("US: VkKeyScanEx('@') is Shift+2", VkKeyScanExW(L'@', HKL_US) == 0x132);
    /* French AZERTY: A and Q, M beside L, digits with Shift */
    CHECK("French: scan code 0x10 is VK_A", MapVirtualKeyExW(0x10, MAPVK_VSC_TO_VK, HKL_FR) == 'A');
    CHECK("French: scan code 0x27 is VK_M", MapVirtualKeyExW(0x27, MAPVK_VSC_TO_VK, HKL_FR) == 'M');
    CHECK("French: the 1 key types &", type(HKL_FR, '1', 0, c) == 1 && c[0] == '&');
    CHECK("French: Shift+1 types 1", type(HKL_FR, '1', 1, c) == 1 && c[0] == '1');
    CHECK("French: the 2 key types é", type(HKL_FR, '2', 0, c) == 1 && c[0] == 0xE9);
    /* German, by handle: the dead circumflex then o */
    UINT circ = MapVirtualKeyExW(0x29, MAPVK_VSC_TO_VK, HKL_DE);
    CHECK("German: ^ is a dead key", (MapVirtualKeyExW(circ, MAPVK_VK_TO_CHAR, HKL_DE) & 0x80000000u) != 0);
    CHECK("German: ^ waits for its letter", type(HKL_DE, circ, 0, c) == -1 && c[0] == '^');
    CHECK("German: ^ o types ô", type(HKL_DE, 'O', 0, c) == 1 && c[0] == 0xF4);
    CHECK("German: ^ space types ^", type(HKL_DE, circ, 0, c) == -1 && type(HKL_DE, VK_SPACE, 0, c) == 1 && c[0] == '^');
    CHECK("LoadKeyboardLayout(\"0000040C\")", (DWORD)(ULONG_PTR)LoadKeyboardLayoutW(L"0000040C", 0) == 0x040C040C);
    CHECK("LoadKeyboardLayoutA(\"00000407\")", (DWORD)(ULONG_PTR)LoadKeyboardLayoutA("00000407", 0) == 0x04070407);

    /* the registry lists them, as Windows does */
    char text[64];
    DWORD n = sizeof(text);
    CHECK("Keyboard Layouts\\0000040C is French",
          !RegGetValueA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\0000040C",
                        "Layout Text", RRF_RT_REG_SZ, NULL, text, &n) && !strcmp(text, "French"));
}

static void german(void)
{
    WCHAR c[4], name[KL_NAMELENGTH];
    HKL h = GetKeyboardLayout(0);
    CHECK("GetKeyboardLayout is German", (DWORD)(ULONG_PTR)h == 0x04070407);
    CHECK("GetKeyboardLayoutName is 00000407", GetKeyboardLayoutNameW(name) && !wcscmp(name, L"00000407"));
    HKL spi = NULL;
    CHECK("SPI_GETDEFAULTINPUTLANG", SystemParametersInfoW(SPI_GETDEFAULTINPUTLANG, 0, &spi, 0) && spi == h);
    /* Y and Z swap places */
    CHECK("VK_Z is scan code 0x15", MapVirtualKeyW('Z', MAPVK_VK_TO_VSC) == 0x15);
    CHECK("scan code 0x2C is VK_Y", MapVirtualKeyW(0x2C, MAPVK_VSC_TO_VK) == 'Y');
    BYTE keys[256];
    memset(keys, 0, sizeof(keys));
    CHECK("the key at 0x15 types z", ToUnicode('Z', 0x15, keys, c, 4, 0) == 1 && c[0] == 'z');
    keys[VK_SHIFT] = 0x80;
    CHECK("Shift+Z types Z", ToUnicode('Z', 0x15, keys, c, 4, 0) == 1 && c[0] == 'Z');
    keys[VK_SHIFT] = 0;
    CHECK("VkKeyScan('z') is VK_Z", (VkKeyScanW(L'z') & 0xFF) == 'Z');
    /* the umlauts */
    UINT oe = MapVirtualKeyW(0x27, MAPVK_VSC_TO_VK);
    CHECK("the key right of L types ö", ToUnicode(oe, 0x27, keys, c, 4, 0) == 1 && c[0] == 0xF6);
    /* AltGr (Ctrl+Alt) */
    keys[VK_CONTROL] = keys[VK_MENU] = 0x80;
    CHECK("AltGr+Q types @", ToUnicode('Q', 0x10, keys, c, 4, 0) == 1 && c[0] == '@');
    CHECK("AltGr+E types €", ToUnicode('E', 0x12, keys, c, 4, 0) == 1 && c[0] == 0x20AC);
    CHECK("VkKeyScan('@') is AltGr+Q", (USHORT)VkKeyScanW(L'@') == 0x651);
    keys[VK_MENU] = 0;
    CHECK("Ctrl+A is ^A", ToUnicode('A', 0x1E, keys, c, 4, 0) == 1 && c[0] == 1);
    keys[VK_CONTROL] = 0;
    /* the dead acute accent */
    UINT acute = MapVirtualKeyW(0x0D, MAPVK_VSC_TO_VK);
    CHECK("´ is a dead key", ToUnicode(acute, 0x0D, keys, c, 4, 0) == -1 && c[0] == 0xB4);
    CHECK("´ e types é", ToUnicode('E', 0x12, keys, c, 4, 0) == 1 && c[0] == 0xE9);
    CHECK("´ again", ToUnicode(acute, 0x0D, keys, c, 4, 0) == -1);
    CHECK("´ x types ´x", ToUnicode('X', 0x2D, keys, c, 4, 0) == 2 && c[0] == 0xB4 && c[1] == 'x');
    /* key names */
    CHECK("GetKeyNameText(0x15) is Z", GetKeyNameTextW(0x15 << 16, name, KL_NAMELENGTH) == 1 && name[0] == 'Z');
    /* where the choice is kept */
    char klid[16];
    DWORD n = sizeof(klid);
    CHECK("Preload 1 is 00000407", !RegGetValueA(HKEY_CURRENT_USER, "Keyboard Layout\\Preload", "1", RRF_RT_REG_SZ,
                                                 NULL, klid, &n) && !strcmp(klid, "00000407"));
}

/* SPI_SETDEFAULTINPUTLANG: the user's layout, which the kernel picks up
 * from the registry and publishes back to every program */
static void back_to_us(void)
{
    HKL us = HKL_US;
    CHECK("SPI_SETDEFAULTINPUTLANG US", SystemParametersInfoW(SPI_SETDEFAULTINPUTLANG, 0, &us, 0x02 /* SPIF_SENDCHANGE */));
    int ms = 0;
    while ((DWORD)(ULONG_PTR)GetKeyboardLayout(0) != 0x04090409 && ms < 3000) { Sleep(50); ms += 50; }
    CHECK("the system follows to US", (DWORD)(ULONG_PTR)GetKeyboardLayout(0) == 0x04090409);
    printf("[KBD] now %08lX after %d ms\n", (unsigned long)(ULONG_PTR)GetKeyboardLayout(0), ms);
}

int main(int argc, char **argv)
{
    char name[KL_NAMELENGTH] = "";
    GetKeyboardLayoutNameA(name);
    printf("[KBD] layout %s hkl 0x%08lX\n", name, (unsigned long)(ULONG_PTR)GetKeyboardLayout(0));
    by_handle();
    if (argc > 1 && !strcmp(argv[1], "00000407")) german();
    else if (argc > 1) { printf("kbdtest: only 00000407 (German) has checks\n"); fail++; }
    if (argc > 2 && !strcmp(argv[2], "us")) back_to_us();
    printf("kbdtest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

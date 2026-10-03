/*
 * tzutil.exe — Windows' time zone utility
 *
 *   tzutil /g              the time zone's name ("W. Europe Standard Time")
 *   tzutil /s "NAME"       make NAME the time zone (NAME_dstoff: without
 *                          daylight saving time)
 *   tzutil /l              every zone: its display name, then its name
 *
 * The zone is kept in HKLM\SYSTEM\CurrentControlSet\Control\
 * TimeZoneInformation (SetDynamicTimeZoneInformation); the desktop clock
 * follows within a minute, programs started afterwards at once.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define TZS_KEY L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones"

static void display_name(const WCHAR *key, char *out, int cap)
{
    WCHAR path[200], v[128];
    HKEY k;
    DWORD n = sizeof(v) - 2, type;
    out[0] = 0;
    int i = 0;
    for (const WCHAR *p = TZS_KEY L"\\"; *p; p++) path[i++] = *p;
    for (; *key && i < 199; key++) path[i++] = *key;
    path[i] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k)) return;
    if (!RegQueryValueExW(k, L"Display", NULL, &type, (BYTE *)v, &n) && type == REG_SZ) {
        v[n / 2] = 0;
        WideCharToMultiByte(CP_UTF8, 0, v, -1, out, cap, NULL, NULL);
    }
    RegCloseKey(k);
}

static int usage(void)
{
    printf("Windows Time Zone Utility\n\n"
           "Usage:\n"
           "TZUTIL </? | /g | /s TimeZoneID[_dstoff] | /l>\n\n"
           "Parameters:\n"
           "    /?  Displays usage information.\n\n"
           "    /g  Displays the current time zone ID.\n\n"
           "    /s TimeZoneID[_dstoff]\n"
           "        Sets the current time zone using the specified time zone ID.\n"
           "        The _dstoff suffix disables Daylight Saving Time adjustments\n"
           "        for the time zone (where applicable).\n\n"
           "    /l  Lists all valid time zone IDs and display names. The output will\n"
           "        be:\n"
           "            <display name>\n"
           "            <time zone ID>\n");
    return 0;
}

int main(int argc, char **argv)
{
    DYNAMIC_TIME_ZONE_INFORMATION d;
    char name[160], disp[160];
    if (argc < 2 || !strcmp(argv[1], "/?") || !_stricmp(argv[1], "-?")) return usage();
    if (!_stricmp(argv[1], "/g")) {
        GetDynamicTimeZoneInformation(&d);
        WideCharToMultiByte(CP_UTF8, 0, d.TimeZoneKeyName, -1, name, sizeof(name), NULL, NULL);
        printf("%s%s\n", name, d.DynamicDaylightTimeDisabled ? "_dstoff" : "");
        return 0;
    }
    if (!_stricmp(argv[1], "/l")) {
        for (DWORD i = 0; !EnumDynamicTimeZoneInformation(i, &d); i++) {
            WideCharToMultiByte(CP_UTF8, 0, d.TimeZoneKeyName, -1, name, sizeof(name), NULL, NULL);
            display_name(d.TimeZoneKeyName, disp, sizeof(disp));
            printf("%s\n%s\n\n", disp, name);
        }
        return 0;
    }
    if (!_stricmp(argv[1], "/s") && argc > 2) {
        /* the name may arrive in several pieces when not quoted */
        char want[160] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) strncat(want, " ", sizeof(want) - strlen(want) - 1);
            strncat(want, argv[i], sizeof(want) - strlen(want) - 1);
        }
        BOOL dstoff = FALSE;
        size_t n = strlen(want);
        if (n > 7 && !_stricmp(want + n - 7, "_dstoff")) { want[n - 7] = 0; dstoff = TRUE; }
        for (DWORD i = 0; !EnumDynamicTimeZoneInformation(i, &d); i++) {
            WideCharToMultiByte(CP_UTF8, 0, d.TimeZoneKeyName, -1, name, sizeof(name), NULL, NULL);
            if (_stricmp(name, want)) continue;
            if (dstoff && d.DaylightDate.wMonth) {
                d.DynamicDaylightTimeDisabled = TRUE;
                memset(&d.StandardDate, 0, sizeof(d.StandardDate));
                memset(&d.DaylightDate, 0, sizeof(d.DaylightDate));
                d.DaylightBias = 0;
            }
            if (!SetDynamicTimeZoneInformation(&d)) {
                printf("Error: Access is denied.\n");
                return 1;
            }
            return 0;
        }
        printf("Error: The system cannot find the specified time zone ID \"%s\".\n", want);
        return 1;
    }
    printf("Error: Invalid parameter or option: \"%s\".\n", argv[1]);
    return 1;
}

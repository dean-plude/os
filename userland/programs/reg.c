/*
 * reg.exe — the registry console tool.
 *
 *   reg query  KEY [/v NAME | /ve] [/s]
 *   reg add    KEY [/v NAME | /ve] [/t TYPE] [/d DATA] [/s SEP] [/f]
 *   reg delete KEY [/v NAME | /ve | /va] [/f]
 *   reg export KEY FILE [/y]
 *
 * KEY starts with a root: HKLM, HKCU, HKCR, HKU, HKCC (or the long names).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *s, *l; HKEY k; } roots[] = {
    { "HKLM", "HKEY_LOCAL_MACHINE",  HKEY_LOCAL_MACHINE },
    { "HKCU", "HKEY_CURRENT_USER",   HKEY_CURRENT_USER },
    { "HKCR", "HKEY_CLASSES_ROOT",   HKEY_CLASSES_ROOT },
    { "HKU",  "HKEY_USERS",          HKEY_USERS },
    { "HKCC", "HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG },
};

static const struct { const char *name; DWORD type; } types[] = {
    { "REG_NONE", REG_NONE }, { "REG_SZ", REG_SZ }, { "REG_EXPAND_SZ", REG_EXPAND_SZ },
    { "REG_BINARY", REG_BINARY }, { "REG_DWORD", REG_DWORD }, { "REG_DWORD_BIG_ENDIAN", REG_DWORD_BIG_ENDIAN },
    { "REG_LINK", REG_LINK }, { "REG_MULTI_SZ", REG_MULTI_SZ }, { "REG_QWORD", REG_QWORD },
};

static const char *type_name(DWORD t)
{
    for (size_t i = 0; i < sizeof types / sizeof *types; i++) if (types[i].type == t) return types[i].name;
    return "REG_UNKNOWN";
}

static int usage(void)
{
    fprintf(stderr,
        "usage: reg query  KEY [/v NAME | /ve] [/s]\n"
        "       reg add    KEY [/v NAME | /ve] [/t TYPE] [/d DATA] [/s SEP] [/f]\n"
        "       reg delete KEY [/v NAME | /ve | /va] [/f]\n"
        "       reg export KEY FILE [/y]\n"
        "KEY is ROOT\\path, ROOT one of HKLM HKCU HKCR HKU HKCC\n");
    return 1;
}

static int fail(LSTATUS e, const char *what)
{
    if (e == ERROR_FILE_NOT_FOUND)
        fprintf(stderr, "ERROR: The system was unable to find the specified registry key or value.\n");
    else if (e == ERROR_ACCESS_DENIED)
        fprintf(stderr, "ERROR: Access is denied.\n");
    else
        fprintf(stderr, "ERROR: %s failed (%ld)\n", what, (long)e);
    return 1;
}

/* splits "HKLM\path" into a root and a subpath; *full receives the long form */
static int parse_key(const char *s, HKEY *root, const char **sub, const char **rootname)
{
    size_t n = strcspn(s, "\\");
    for (size_t i = 0; i < sizeof roots / sizeof *roots; i++)
        if ((strlen(roots[i].s) == n && !_strnicmp(s, roots[i].s, n)) ||
            (strlen(roots[i].l) == n && !_strnicmp(s, roots[i].l, n))) {
            *root = roots[i].k;
            *sub = s[n] ? s + n + 1 : "";
            *rootname = roots[i].l;
            return 1;
        }
    fprintf(stderr, "ERROR: Invalid key name.\n");
    return 0;
}

static void print_data(FILE *f, DWORD type, const BYTE *d, DWORD n)
{
    switch (type) {
    case REG_SZ: case REG_EXPAND_SZ: case REG_LINK:
        fprintf(f, "%.*s", (int)n, (const char *)d);
        break;
    case REG_MULTI_SZ: {
        const char *p = (const char *)d, *end = p + n;
        int first = 1;
        while (p < end && *p) {
            fprintf(f, "%s%s", first ? "" : "\\0", p);
            first = 0;
            p += strlen(p) + 1;
        }
        break;
    }
    case REG_DWORD:
        if (n >= 4) fprintf(f, "0x%lx", (unsigned long)*(const DWORD *)d);
        break;
    case REG_DWORD_BIG_ENDIAN:
        if (n >= 4) fprintf(f, "0x%lx", (unsigned long)((DWORD)d[0] << 24 | (DWORD)d[1] << 16 | (DWORD)d[2] << 8 | d[3]));
        break;
    case REG_QWORD:
        if (n >= 8) fprintf(f, "0x%llx", *(const unsigned long long *)d);
        break;
    default:
        for (DWORD i = 0; i < n; i++) fprintf(f, "%02X", d[i]);
    }
}

static void print_value(const char *name, DWORD type, const BYTE *d, DWORD n)
{
    printf("    %s    %s    ", *name ? name : "(Default)", type_name(type));
    print_data(stdout, type, d, n);
    printf("\n");
}

/* prints one key's values (and, with recurse, its subkeys); returns values printed */
static int query_key(HKEY k, const char *path, const char *only, int recurse)
{
    DWORD nvals = 0, maxname = 0, maxdata = 0, nsub = 0, maxsub = 0;
    if (RegQueryInfoKeyA(k, 0, 0, 0, &nsub, &maxsub, 0, &nvals, &maxname, &maxdata, 0, 0)) return 0;
    char *name = malloc(maxname * 4 + 4);           /* sizes are in UTF-16 units; UTF-8 can be 3x */
    BYTE *data = malloc(maxdata * 2 + 4);
    int shown = 0;
    for (DWORD i = 0;; i++) {
        DWORD nn = maxname * 4 + 4, nd = maxdata * 2 + 4, type;
        if (RegEnumValueA(k, i, name, &nn, 0, &type, data, &nd)) break;
        if (only && _stricmp(only, name)) continue;
        if (!shown++) printf("\n%s\n", path);
        print_value(name, type, data, nd);
    }
    free(name);
    free(data);
    if (!only && !recurse && !shown) printf("\n%s\n", path);
    char sub[512];
    for (DWORD i = 0;; i++) {
        DWORD n = sizeof sub;
        if (RegEnumKeyExA(k, i, sub, &n, 0, 0, 0, 0)) break;
        size_t plen = strlen(path) + strlen(sub) + 2;
        char *child = malloc(plen);
        snprintf(child, plen, "%s\\%s", path, sub);
        if (recurse) {
            HKEY c;
            if (!RegOpenKeyExA(k, sub, 0, KEY_READ, &c)) {
                shown += query_key(c, child, only, 1);
                RegCloseKey(c);
            }
        } else if (!only) {
            if (!i) printf("\n");
            printf("%s\n", child);
        }
        free(child);
    }
    return shown;
}

static int cmd_query(int argc, char **argv)
{
    HKEY root, k;
    const char *sub, *rootname, *only = 0;
    int recurse = 0;
    if (argc < 1 || !parse_key(argv[0], &root, &sub, &rootname)) return usage();
    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "/v") && i + 1 < argc) only = argv[++i];
        else if (!_stricmp(argv[i], "/ve")) only = "";
        else if (!_stricmp(argv[i], "/s")) recurse = 1;
        else return usage();
    }
    LSTATUS e = RegOpenKeyExA(root, sub, 0, KEY_READ, &k);
    if (e) return fail(e, "open");
    char path[1024];
    snprintf(path, sizeof path, "%s%s%s", rootname, *sub ? "\\" : "", sub);
    int shown = query_key(k, path, only, recurse);
    RegCloseKey(k);
    if (only && !shown) return fail(ERROR_FILE_NOT_FOUND, "query");
    if (recurse) printf("\nEnd of search: %d match(es) found.\n", shown);
    return 0;
}

static int cmd_add(int argc, char **argv)
{
    HKEY root, k;
    const char *sub, *rootname, *name = 0, *data = 0, *sep = "\\0";
    DWORD type = REG_SZ;
    int have_type = 0;
    if (argc < 1 || !parse_key(argv[0], &root, &sub, &rootname)) return usage();
    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "/v") && i + 1 < argc) name = argv[++i];
        else if (!_stricmp(argv[i], "/ve")) name = "";
        else if (!_stricmp(argv[i], "/d") && i + 1 < argc) data = argv[++i];
        else if (!_stricmp(argv[i], "/s") && i + 1 < argc) sep = argv[++i];
        else if (!_stricmp(argv[i], "/f")) ;
        else if (!_stricmp(argv[i], "/t") && i + 1 < argc) {
            const char *t = argv[++i];
            size_t j;
            for (j = 0; j < sizeof types / sizeof *types; j++) if (!_stricmp(t, types[j].name)) break;
            if (j == sizeof types / sizeof *types) { fprintf(stderr, "ERROR: Invalid type %s\n", t); return 1; }
            type = types[j].type;
            have_type = 1;
        } else return usage();
    }
    LSTATUS e = RegCreateKeyExA(root, sub, 0, 0, 0, KEY_WRITE, 0, &k, 0);
    if (e) return fail(e, "create");
    if (name) {
        if (!have_type && !data) type = REG_SZ;
        const char *d = data ? data : "";
        BYTE *buf = 0;
        DWORD n = 0;
        switch (type) {
        case REG_DWORD: case REG_DWORD_BIG_ENDIAN: {
            DWORD v = (DWORD)strtoul(d, 0, 0);
            if (type == REG_DWORD_BIG_ENDIAN) v = v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24;
            buf = malloc(4); memcpy(buf, &v, 4); n = 4;
            break;
        }
        case REG_QWORD: {
            unsigned long long v = strtoull(d, 0, 0);
            buf = malloc(8); memcpy(buf, &v, 8); n = 8;
            break;
        }
        case REG_BINARY: case REG_NONE: {
            size_t l = strlen(d);
            buf = malloc(l / 2 + 1);
            for (size_t j = 0; j + 1 < l + 1 && d[j] && d[j + 1]; j += 2) {
                char hx[3] = { d[j], d[j + 1], 0 };
                buf[n++] = (BYTE)strtoul(hx, 0, 16);
            }
            break;
        }
        case REG_MULTI_SZ: {
            size_t l = strlen(d), sl = strlen(sep);
            buf = malloc(l + 2);
            for (size_t j = 0; j < l;) {
                if (sl && !strncmp(d + j, sep, sl)) { buf[n++] = 0; j += sl; }
                else buf[n++] = (BYTE)d[j++];
            }
            if (l) buf[n++] = 0;
            buf[n++] = 0;
            break;
        }
        default:
            n = (DWORD)strlen(d) + 1;
            buf = malloc(n);
            memcpy(buf, d, n);
        }
        e = RegSetValueExA(k, name, 0, type, buf, n);
        free(buf);
    }
    RegCloseKey(k);
    if (e) return fail(e, "set");
    printf("The operation completed successfully.\n");
    return 0;
}

static int cmd_delete(int argc, char **argv)
{
    HKEY root, k;
    const char *sub, *rootname, *name = 0;
    int all = 0;
    if (argc < 1 || !parse_key(argv[0], &root, &sub, &rootname)) return usage();
    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "/v") && i + 1 < argc) name = argv[++i];
        else if (!_stricmp(argv[i], "/ve")) name = "";
        else if (!_stricmp(argv[i], "/va")) all = 1;
        else if (!_stricmp(argv[i], "/f")) ;
        else return usage();
    }
    LSTATUS e;
    if (!name && !all) {
        if (!*sub) { fprintf(stderr, "ERROR: Cannot delete a root key.\n"); return 1; }
        e = RegDeleteTreeA(root, sub);      /* the key, its values and subkeys */
        if (e) return fail(e, "delete");
    } else {
        e = RegOpenKeyExA(root, sub, 0, KEY_READ | KEY_WRITE, &k);
        if (e) return fail(e, "open");
        if (all) {
            char vn[512];
            for (;;) {
                DWORD n = sizeof vn;
                if (RegEnumValueA(k, 0, vn, &n, 0, 0, 0, 0)) break;
                if ((e = RegDeleteValueA(k, vn))) break;
            }
        } else e = RegDeleteValueA(k, name);
        RegCloseKey(k);
        if (e) return fail(e, "delete");
    }
    printf("The operation completed successfully.\n");
    return 0;
}

/* .reg text: one [key] section per key, "name"=data lines, as regedit writes it */
static void export_str(FILE *f, const char *s, size_t n)
{
    fputc('"', f);
    for (size_t i = 0; i < n && s[i]; i++) {
        if (s[i] == '"' || s[i] == '\\') fputc('\\', f);
        fputc(s[i], f);
    }
    fputc('"', f);
}

static void export_key(FILE *f, HKEY k, const char *path)
{
    fprintf(f, "\n[%s]\n", path);
    DWORD maxname = 0, maxdata = 0;
    RegQueryInfoKeyA(k, 0, 0, 0, 0, 0, 0, 0, &maxname, &maxdata, 0, 0);
    char *name = malloc(maxname * 4 + 4);
    BYTE *data = malloc(maxdata * 2 + 4);
    for (DWORD i = 0;; i++) {
        DWORD nn = maxname * 4 + 4, nd = maxdata * 2 + 4, type;
        if (RegEnumValueA(k, i, name, &nn, 0, &type, data, &nd)) break;
        if (*name) export_str(f, name, nn); else fputc('@', f);
        fputc('=', f);
        if (type == REG_SZ) export_str(f, (char *)data, nd);
        else if (type == REG_DWORD && nd == 4) fprintf(f, "dword:%08lx", (unsigned long)*(DWORD *)data);
        else {
            if (type == REG_BINARY) fprintf(f, "hex:");
            else fprintf(f, "hex(%lx):", (unsigned long)type);
            for (DWORD j = 0; j < nd; j++) fprintf(f, "%02x%s", data[j], j + 1 < nd ? "," : "");
        }
        fputc('\n', f);
    }
    free(name);
    free(data);
    char sub[512];
    for (DWORD i = 0;; i++) {
        DWORD n = sizeof sub;
        if (RegEnumKeyExA(k, i, sub, &n, 0, 0, 0, 0)) break;
        HKEY c;
        if (RegOpenKeyExA(k, sub, 0, KEY_READ, &c)) continue;
        size_t plen = strlen(path) + strlen(sub) + 2;
        char *child = malloc(plen);
        snprintf(child, plen, "%s\\%s", path, sub);
        export_key(f, c, child);
        free(child);
        RegCloseKey(c);
    }
}

static int cmd_export(int argc, char **argv)
{
    HKEY root, k;
    const char *sub, *rootname;
    if (argc < 2 || !parse_key(argv[0], &root, &sub, &rootname)) return usage();
    LSTATUS e = RegOpenKeyExA(root, sub, 0, KEY_READ, &k);
    if (e) return fail(e, "open");
    FILE *f = fopen(argv[1], "w");
    if (!f) { perror(argv[1]); RegCloseKey(k); return 1; }
    char path[1024];
    snprintf(path, sizeof path, "%s%s%s", rootname, *sub ? "\\" : "", sub);
    fprintf(f, "Windows Registry Editor Version 5.00\n");
    export_key(f, k, path);
    fclose(f);
    RegCloseKey(k);
    printf("The operation completed successfully.\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *c = argv[1];
    if (!_stricmp(c, "query"))  return cmd_query(argc - 2, argv + 2);
    if (!_stricmp(c, "add"))    return cmd_add(argc - 2, argv + 2);
    if (!_stricmp(c, "delete")) return cmd_delete(argc - 2, argv + 2);
    if (!_stricmp(c, "export")) return cmd_export(argc - 2, argv + 2);
    return usage();
}

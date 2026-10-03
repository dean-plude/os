/*
 * install.c — the installation engine
 *
 * Runs the package's InstallExecuteSequence: the costing actions select
 * features and components and resolve the Directory table to paths, then
 * InstallFiles extracts the cabinets, WriteRegistryValues writes the
 * Registry table, and InstallFinalize registers the product under the
 * Uninstall key and keeps a copy of the package for uninstalling.
 * Custom actions run: property and directory setters, DLLs (in a
 * custom-action server, see api.c), programs, deferred ones in their
 * place in the sequence and commit ones at the end.  CreateShortcuts
 * makes .lnk files, InstallServices registers services with the service
 * control manager.  With full UI the package's own dialogs run first
 * (InstallUISequence, dialog.c).  Uninstalling (REMOVE=ALL) reverses the
 * files, folders, registry entries, shortcuts and services.
 */
#define MSI_EXPORT __declspec(dllexport)
#include "msi.h"
#include "engine.h"
#include "ui.h"
#include "dialog.h"
#include <objbase.h>

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
typedef struct { char *name, *value; } Prop;

struct Inst {
    MsiDb     db;
    void     *pkg;
    size_t    pkg_size;
    WCHAR     pkg_path[MAX_PATH];
    char      source_dir[MAX_PATH];  /* the package's folder (UTF-8, trailing '\') */
    Prop     *props;
    int       nprops, props_cap;
    MsiTable *feature, *component, *file, *dir, *media;
    bool     *feature_on;
    bool     *comp_on;
    bool      remove;
    int       ui_level;
    MsiUi    *ui;
    FILE     *log;
    int       total_work, done_work;
    char      error[256];
    int       result;
    bool      resolved;              /* directories resolved (CostFinalize ran) */
    bool      removed_files, removed_registry, removed_folders, removed_env, removed_shortcuts;
    bool     *dir_explicit;          /* Directory rows whose path was set, not derived */
    bool      installed;             /* the product was registered before this run */
    char     *installed_features;    /* ",A,B," as registered, NULL if unknown */
    bool      modes[32];             /* MsiSetMode / MsiGetMode */
    char    **commit;                /* commit custom actions, run after InstallFinalize */
    int       ncommit;
    char    **ran;                   /* actions the UI sequence ran (FirstSequence, OncePerProcess) */
    int       nran;
    bool      in_ui;                 /* running InstallUISequence */
    bool      executed;              /* ExecuteAction ran */
    bool      stop_sequence;         /* a custom action returned ERROR_NO_MORE_ITEMS */
    Dlg      *dlg;                   /* the package's dialogs (full UI) */
    char      action_template[512];  /* ActionText template of the running action */
    long long prog_total, prog_done; /* progress ticks from MsiProcessMessage */
    int       sequence_depth;
    const WCHAR *cmdline;            /* the command line's properties */
    struct RbOp *rb;                 /* the rollback journal */
    int       nrb, rb_cap, rb_serial;
    bool      rb_on;                 /* changes are journaled */
    bool      rolling_back;
    char     *xforms;                /* transforms applied: their paths, ';' separated */
    char     *patches;               /* patches applied: their paths, ';' separated */
    char      patch_remove[64];      /* MSIPATCHREMOVE: the patch this run takes off */
    char      removed_patch[MAX_PATH]; /* ... and its kept copy, deleted once it is off */
};

static void cache_changes(Inst *in, HKEY h);

static void logf(Inst *in, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    char out[1040];
    snprintf(out, sizeof(out), "MSI: %s\n", line);
    OutputDebugStringA(out);
    if (in->log) { fputs(line, in->log); fputs("\r\n", in->log); fflush(in->log); }
}

static void fail(Inst *in, int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(in->error, sizeof(in->error), fmt, ap);
    va_end(ap);
    if (!in->result) in->result = code;
    logf(in, "Error: %s", in->error);
}

/* UTF-8 <-> UTF-16 */
static void to_w(const char *s, WCHAR *out, int cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    if (n <= 0 && cap) out[0] = 0;
    out[cap - 1] = 0;
}

static void to_u8(const WCHAR *s, char *out, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL);
    if (n <= 0 && cap) out[0] = 0;
    out[cap - 1] = 0;
}

/* -----------------------------------------------------------------------
 * Properties
 * ----------------------------------------------------------------------- */
static const char *get_prop(Inst *in, const char *name)
{
    for (int i = 0; i < in->nprops; i++)
        if (!strcmp(in->props[i].name, name)) return in->props[i].value;
    if (name[0] == '%') {                        /* environment variable */
        static char env[512];
        WCHAR wn[128], wv[512];
        to_w(name + 1, wn, 128);
        DWORD n = GetEnvironmentVariableW(wn, wv, 512);
        if (!n || n >= 512) return "";
        to_u8(wv, env, sizeof(env));
        return env;
    }
    return "";
}

static void set_prop(Inst *in, const char *name, const char *value)
{
    for (int i = 0; i < in->nprops; i++) {
        if (strcmp(in->props[i].name, name)) continue;
        free(in->props[i].value);
        in->props[i].value = strdup(value ? value : "");
        return;
    }
    if (in->nprops == in->props_cap) {
        in->props_cap = in->props_cap ? in->props_cap * 2 : 64;
        in->props = realloc(in->props, (size_t)in->props_cap * sizeof(Prop));
    }
    in->props[in->nprops].name = strdup(name);
    in->props[in->nprops].value = strdup(value ? value : "");
    in->nprops++;
}

static bool prop_set(Inst *in, const char *name) { return get_prop(in, name)[0] != 0; }

static const char *state_prop(Inst *in, const char *name);
static const char *cond_prop(void *ctx, const char *name)
{
    if (name[0] == '&' || name[0] == '!' || name[0] == '$' || name[0] == '?') return state_prop(ctx, name);
    return get_prop(ctx, name);
}
static bool cond(Inst *in, const char *c) { return msi_condition(c, cond_prop, in); }

/* "NAME=value NAME2="two words"" from the command line */
static void parse_cmdline_props(Inst *in, const WCHAR *cmd)
{
    if (!cmd) return;
    char u8[4096];
    to_u8(cmd, u8, sizeof(u8));
    char *p = u8;
    while (*p) {
        while (*p == ' ') p++;
        char *name = p;
        while (*p && *p != '=' && *p != ' ') p++;
        if (*p != '=') { while (*p && *p != ' ') p++; continue; }
        *p++ = '\0';
        char value[2048];
        int n = 0;
        if (*p == '"') {
            p++;
            while (*p && !(*p == '"' && p[1] != '"') && n < (int)sizeof(value) - 1) {
                if (*p == '"' && p[1] == '"') p++;
                value[n++] = *p++;
            }
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ' && n < (int)sizeof(value) - 1) value[n++] = *p++;
        }
        value[n] = '\0';
        if (*name) set_prop(in, name, value);
    }
}

/* -----------------------------------------------------------------------
 * Formatted strings: [Property], [#File], [$Component], [%ENV], [\c], [~]
 * ----------------------------------------------------------------------- */
static void dir_path(Inst *in, const char *dir, char *out, int cap);
static void file_path(Inst *in, const char *filekey, char *out, int cap);

/* One [...] key; @missing is set when it names an empty property (for
 * {...} groups) */
static const char *format_key(Inst *in, const MsiRec *rec, const char *key, char *val, int cap, bool *missing)
{
    const char *v = "";
    if (key[0] == '\\' && key[1]) { val[0] = key[1]; val[1] = 0; return val; }
    if (!strcmp(key, "~")) return "";                          /* null separator: dropped */
    if (key[0] == '#' || key[0] == '!') { file_path(in, key + 1, val, cap); v = val; }
    else if (key[0] == '$') { dir_path(in, key + 1, val, cap); v = val; }
    else if (key[0] == '%') v = get_prop(in, key);
    else if (key[0] >= '0' && key[0] <= '9') {
        char nb[16];
        v = rec ? msirec_str(rec, atoi(key), nb) : "";
        snprintf(val, (size_t)cap, "%s", v);
        v = val;
    } else if (key[0]) v = get_prop(in, key);
    if (!*v) *missing = true;
    return v;
}

static void format_rec(Inst *in, const MsiRec *rec, const char *s, char *out, int cap)
{
    int n = 0;
    while (*s && n < cap - 1) {
        if (*s == '{' && s[1] != '{') {
            /* {text [PROP]}: the text only when every property in it is set */
            const char *e = strchr(s + 1, '}');
            if (!e) { out[n++] = *s++; continue; }
            char inner[1024], fmt[2048];
            size_t il = (size_t)(e - s - 1);
            if (il >= sizeof(inner)) il = sizeof(inner) - 1;
            memcpy(inner, s + 1, il);
            inner[il] = 0;
            s = e + 1;
            bool missing = false, any = false;
            int k = 0;
            for (const char *p = inner; *p && k < (int)sizeof(fmt) - 1; ) {
                const char *q;
                if (*p == '[' && (q = strchr(p + 1, ']'))) {
                    char key[256], val[MAX_PATH * 2];
                    size_t kl = (size_t)(q - p - 1);
                    if (kl >= sizeof(key)) kl = sizeof(key) - 1;
                    memcpy(key, p + 1, kl);
                    key[kl] = 0;
                    any = true;
                    const char *v = format_key(in, rec, key, val, sizeof(val), &missing);
                    for (; *v && k < (int)sizeof(fmt) - 1; v++) fmt[k++] = *v;
                    p = q + 1;
                } else fmt[k++] = *p++;
            }
            fmt[k] = 0;
            if (any && missing) continue;
            if (!any && !il) continue;                          /* "{}" */
            for (const char *v = fmt; *v && n < cap - 1; v++) out[n++] = *v;
            continue;
        }
        if (*s != '[') { out[n++] = *s++; continue; }
        const char *e = strchr(s + 1, ']');
        if (!e) { out[n++] = *s++; continue; }
        char key[256];
        size_t kl = (size_t)(e - s - 1);
        if (kl >= sizeof(key)) kl = sizeof(key) - 1;
        memcpy(key, s + 1, kl);
        key[kl] = '\0';
        if (strchr(key, '[')) {                                 /* nested: [[NAME]] */
            char inner[512];
            const char *e2 = strchr(e + 1, ']');
            snprintf(inner, sizeof(inner), "%.*s", (int)(e - s), s + 1);
            if (e2) {
                char resolved[512];
                format_rec(in, rec, inner, resolved, sizeof(resolved));
                char val[MAX_PATH * 2];
                bool missing = false;
                const char *v = format_key(in, rec, resolved, val, sizeof(val), &missing);
                for (; *v && n < cap - 1; v++) out[n++] = *v;
                s = e2 + 1;
                continue;
            }
        }
        s = e + 1;
        char val[MAX_PATH * 2];
        bool missing = false;
        const char *v = format_key(in, rec, key, val, sizeof(val), &missing);
        for (; *v && n < cap - 1; v++) out[n++] = *v;
    }
    out[n] = '\0';
}

static void format_str(Inst *in, const char *s, char *out, int cap) { format_rec(in, NULL, s, out, cap); }

/* -----------------------------------------------------------------------
 * Directories
 * ----------------------------------------------------------------------- */
/* The long name of a "short|long" pair, or the "target" half of
 * "target:source" */
static void long_name(const char *dd, char *out, int cap)
{
    char part[MAX_PATH];
    const char *colon = strchr(dd, ':');
    size_t n = colon ? (size_t)(colon - dd) : strlen(dd);
    if (n >= sizeof(part)) n = sizeof(part) - 1;
    memcpy(part, dd, n);
    part[n] = '\0';
    const char *bar = strchr(part, '|');
    const char *name = bar ? bar + 1 : part;
    snprintf(out, (size_t)cap, "%s", name);
}

static void ensure_slash(char *p, int cap)
{
    size_t n = strlen(p);
    if (n && p[n - 1] != '\\' && (int)n < cap - 1) { p[n] = '\\'; p[n + 1] = 0; }
}

static void dir_path(Inst *in, const char *dir, char *out, int cap)
{
    const char *p = get_prop(in, dir);
    if (*p) { snprintf(out, (size_t)cap, "%s", p); ensure_slash(out, cap); return; }
    int r = msidb_find(&in->db, in->dir, 0, dir, 0);
    if (r < 0) { snprintf(out, (size_t)cap, "C:\\%s\\", dir); return; }
    char b[16];
    const char *parent = msidb_str(&in->db, in->dir, r, 1, b);
    char name[MAX_PATH];
    long_name(msidb_str(&in->db, in->dir, r, 2, b), name, sizeof(name));
    if (!*parent || !strcmp(parent, dir)) {
        /* a root: TARGETDIR (SourceDir) */
        snprintf(out, (size_t)cap, "%s", *get_prop(in, "TARGETDIR") ? get_prop(in, "TARGETDIR") : "C:\\");
    } else {
        char pp[MAX_PATH];
        dir_path(in, parent, pp, sizeof(pp));
        if (!strcmp(name, ".") || !name[0]) snprintf(out, (size_t)cap, "%s", pp);
        else snprintf(out, (size_t)cap, "%s%s", pp, name);
    }
    ensure_slash(out, cap);
}

static void file_path(Inst *in, const char *filekey, char *out, int cap)
{
    int r = msidb_find(&in->db, in->file, 0, filekey, 0);
    if (r < 0) { out[0] = 0; return; }
    char b[16];
    const char *comp = msidb_str(&in->db, in->file, r, 1, b);
    int c = msidb_find(&in->db, in->component, 0, comp, 0);
    char dp[MAX_PATH] = "";
    if (c >= 0) dir_path(in, msidb_str(&in->db, in->component, c, 2, b), dp, sizeof(dp));
    char name[MAX_PATH];
    long_name(msidb_str(&in->db, in->file, r, 2, b), name, sizeof(name));
    snprintf(out, (size_t)cap, "%s%s", dp, name);
}

static void standard_folders(Inst *in)
{
    static const struct { const char *name, *path; } f[] = {
        { "ProgramFilesFolder", "C:\\Programs\\" }, { "ProgramFiles64Folder", "C:\\Programs\\" },
        { "CommonFilesFolder", "C:\\Programs\\Common Files\\" }, { "CommonFiles64Folder", "C:\\Programs\\Common Files\\" },
        { "SystemFolder", "C:\\Windows\\System32\\" }, { "System64Folder", "C:\\Windows\\System32\\" },
        { "System16Folder", "C:\\Windows\\System32\\" }, { "WindowsFolder", "C:\\Windows\\" },
        { "WindowsVolume", "C:\\" }, { "FontsFolder", "C:\\Windows\\Fonts\\" },
        { "TempFolder", "C:\\Temp\\" }, { "DesktopFolder", "C:\\Desktop\\" },
        /* one user: the Start menu lists C:\AppData\Roaming\Start Menu\Programs */
        { "ProgramMenuFolder", "C:\\AppData\\Roaming\\Start Menu\\Programs\\" },
        { "StartMenuFolder", "C:\\AppData\\Roaming\\Start Menu\\" },
        { "StartupFolder", "C:\\AppData\\Roaming\\Start Menu\\Programs\\Startup\\" },
        { "AppDataFolder", "C:\\AppData\\Roaming\\" }, { "LocalAppDataFolder", "C:\\AppData\\Local\\" },
        { "CommonAppDataFolder", "C:\\ProgramData\\" }, { "PersonalFolder", "C:\\Documents\\" },
        { "MyPicturesFolder", "C:\\Pictures\\" }, { "SendToFolder", "C:\\AppData\\Roaming\\SendTo\\" },
        { "TemplateFolder", "C:\\AppData\\Roaming\\Templates\\" }, { "FavoritesFolder", "C:\\Favorites\\" },
        { "NetHoodFolder", "C:\\AppData\\Roaming\\NetHood\\" }, { "PrintHoodFolder", "C:\\AppData\\Roaming\\PrintHood\\" },
        { "RecentFolder", "C:\\AppData\\Roaming\\Recent\\" }, { "AdminToolsFolder", "C:\\AppData\\Roaming\\Start Menu\\Programs\\Administrative Tools\\" },
        { "LocalAppDataFolder", "C:\\AppData\\Local\\" },
    };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++)
        if (!prop_set(in, f[i].name)) set_prop(in, f[i].name, f[i].path);
}

static void system_properties(Inst *in)
{
    set_prop(in, "VersionNT", "1000");
    set_prop(in, "VersionNT64", "1000");
    set_prop(in, "WindowsBuild", "19045");
    set_prop(in, "ServicePackLevel", "0");
    set_prop(in, "MsiNTProductType", "1");
    set_prop(in, "Privileged", "1");
    set_prop(in, "AdminUser", "1");
    set_prop(in, "MsiRunningElevated", "1");
    set_prop(in, "Msix64", "6");
    set_prop(in, "Intel", "6");
    set_prop(in, "VersionMsi", "5.00");
    set_prop(in, "VersionDatabase", "500");
    set_prop(in, "PhysicalMemory", "2048");
    set_prop(in, "ScreenX", "1280");
    set_prop(in, "ScreenY", "800");
    set_prop(in, "ColorBits", "32");
    set_prop(in, "SystemLanguageID", "1033");
    set_prop(in, "UserLanguageID", "1033");
    set_prop(in, "UserSID", "S-1-5-21-0-0-0-1001");
    set_prop(in, "LogonUser", "dean");
    set_prop(in, "USERNAME", "dean");
    set_prop(in, "COMPANYNAME", "");
    set_prop(in, "ComputerName", "NOVA-PC");
    set_prop(in, "MsiSystemRebootPending", "");
    SYSTEMTIME st;
    GetLocalTime(&st);
    char d[32];
    snprintf(d, sizeof(d), "%02u/%02u/%04u", st.wMonth, st.wDay, st.wYear);
    set_prop(in, "Date", d);
    snprintf(d, sizeof(d), "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    set_prop(in, "Time", d);
    char ui[4];
    snprintf(ui, sizeof(ui), "%d", in->ui_level);
    set_prop(in, "UILevel", ui);
    set_prop(in, "SourceDir", in->source_dir);
    set_prop(in, "OriginalDatabase", in->source_dir);   /* (folder; the file is appended below) */
    char p[MAX_PATH];
    to_u8(in->pkg_path, p, sizeof(p));
    set_prop(in, "OriginalDatabase", p);
    set_prop(in, "DATABASE", p);
    if (!prop_set(in, "INSTALLLEVEL")) set_prop(in, "INSTALLLEVEL", "1");
    if (!prop_set(in, "ALLUSERS")) set_prop(in, "ALLUSERS", "1");
    if (!prop_set(in, "TARGETDIR")) set_prop(in, "TARGETDIR", "C:\\");
    standard_folders(in);
}

/* -----------------------------------------------------------------------
 * Registry helpers
 * ----------------------------------------------------------------------- */
static HKEY root_key(Inst *in, int root)
{
    switch (root) {
    case 0:  return HKEY_CLASSES_ROOT;
    case 1:  return HKEY_CURRENT_USER;
    case 2:  return HKEY_LOCAL_MACHINE;
    case 3:  return HKEY_USERS;
    default: return prop_set(in, "ALLUSERS") ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    }
}

#define NOVA_INSTALLER_KEY L"SOFTWARE\\NovaOS\\Installer\\Products"
#define UNINSTALL_KEY      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"

static bool product_registered(const char *code, WCHAR *local_package, int cap)
{
    WCHAR key[300], wc[64];
    to_w(code, wc, 64);
    _snwprintf(key, 300, L"%s\\%s", NOVA_INSTALLER_KEY, wc);
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h)) return false;
    if (local_package) {
        DWORD type, size = (DWORD)cap * 2;
        if (RegQueryValueExW(h, L"LocalPackage", NULL, &type, (BYTE *)local_package, &size)) local_package[0] = 0;
    }
    RegCloseKey(h);
    return true;
}

static const char *g_install_keys[] = { "INSTALLDIR", "INSTALLLOCATION", "APPLICATIONFOLDER", "INSTALLFOLDER" };

/* Where a registered product went (its main directory property), so a
 * removal works on the folder chosen at install time */
static void restore_install_location(Inst *in, const char *code)
{
    WCHAR key[300], wc[64], loc[MAX_PATH];
    to_w(code, wc, 64);
    _snwprintf(key, 300, L"%s\\%s", NOVA_INSTALLER_KEY, wc);
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h)) return;
    DWORD type, size = sizeof(loc);
    bool ok = !RegQueryValueExW(h, L"InstallLocation", NULL, &type, (BYTE *)loc, &size) && loc[0];
    RegCloseKey(h);
    if (!ok) return;
    char u8[MAX_PATH];
    to_u8(loc, u8, sizeof(u8));
    for (size_t i = 0; i < sizeof(g_install_keys) / sizeof(g_install_keys[0]); i++) {
        if (prop_set(in, g_install_keys[i])) return;             /* given on the command line */
        if (msidb_find(&in->db, in->dir, 0, g_install_keys[i], 0) >= 0) {
            set_prop(in, g_install_keys[i], u8);
            logf(in, "%s = %s (from the registration)", g_install_keys[i], u8);
            return;
        }
    }
}

/* -----------------------------------------------------------------------
 * Costing: features and components
 * ----------------------------------------------------------------------- */
static int find_feature(Inst *in, const char *name) { return msidb_find(&in->db, in->feature, 0, name, 0); }

static bool in_list(const char *list, const char *name)
{
    /* "ALL" or "a,b,c" */
    if (!list || !*list) return false;
    if (!strcmp(list, "ALL")) return true;
    size_t n = strlen(name);
    for (const char *p = list; *p; ) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && !strncmp(p, name, n)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static bool feature_was_installed(Inst *in, const char *name)
{
    if (!in->installed) return false;
    if (!in->installed_features) return true;              /* registered before features were recorded */
    char k[100];
    snprintf(k, sizeof(k), ",%s,", name);
    return strstr(in->installed_features, k) != NULL;
}

/* Components follow their features (and their own conditions) */
static void compute_components(Inst *in)
{
    MsiTable *comp = in->component, *fc = msidb_table(&in->db, "FeatureComponents");
    int nc = comp ? comp->nrows : 0;
    if (!in->comp_on) in->comp_on = calloc((size_t)nc + 1, 1);
    int ccond = msidb_col(comp, "Condition");
    char b[16];
    for (int c = 0; c < nc; c++) {
        const char *cname = msidb_str(&in->db, comp, c, 0, b);
        bool on = false;
        for (int r = 0; fc && r < fc->nrows; r++) {
            if (strcmp(msidb_str(&in->db, fc, r, 1, b), cname)) continue;
            int f = find_feature(in, msidb_str(&in->db, fc, r, 0, b));
            if (f >= 0 && in->feature_on[f]) on = true;
        }
        if (on && !in->remove && !cond(in, msidb_str(&in->db, comp, c, ccond, b))) on = false;
        in->comp_on[c] = on;
    }
}

static void select_features(Inst *in)
{
    MsiTable *ft = in->feature;
    int nf = ft ? ft->nrows : 0;
    free(in->feature_on);
    in->feature_on = calloc((size_t)nf + 1, 1);
    int level = atoi(get_prop(in, "INSTALLLEVEL"));
    if (level <= 0) level = 1;
    /* the Condition table changes feature levels */
    MsiTable *ct = msidb_table(&in->db, "Condition");
    char b[16];
    const char *add = get_prop(in, "ADDLOCAL"), *rem = get_prop(in, "REMOVE");
    bool explicit_list = *add && strcmp(add, "ALL");
    for (int i = 0; i < nf; i++) {
        const char *name = msidb_str(&in->db, ft, i, 0, b);
        int lv = msidb_int(&in->db, ft, i, msidb_col(ft, "Level"), NULL);
        for (int r = 0; ct && r < ct->nrows; r++) {
            if (strcmp(msidb_str(&in->db, ct, r, 0, b), name)) continue;
            int nl = msidb_int(&in->db, ct, r, 1, NULL);
            if (cond(in, msidb_str(&in->db, ct, r, 2, b))) lv = nl;
        }
        bool on = lv > 0 && lv <= level;
        if (in->installed && !in->remove) on = feature_was_installed(in, name);   /* maintenance: keep what is there */
        if (explicit_list && !in->installed) on = false;   /* ADDLOCAL=a,b: only those (and their parents) */
        if (in_list(add, name)) on = true;
        if (in_list(rem, name)) on = false;
        if (in->remove) on = feature_was_installed(in, name) || !in->installed_features;
        in->feature_on[i] = on;
    }
    /* a feature asked for brings its parents; a feature whose parent is off is off */
    for (int pass = 0; pass < 8; pass++)
        for (int i = 0; i < nf; i++) {
            const char *parent = msidb_str(&in->db, ft, i, 1, b);
            if (!*parent || !in->feature_on[i]) continue;
            int p = find_feature(in, parent);
            if (p < 0 || p == i || in->feature_on[p]) continue;
            if (in_list(add, msidb_str(&in->db, ft, i, 0, b)) && !in_list(rem, parent)) in->feature_on[p] = true;
            else in->feature_on[i] = false;
        }
    free(in->comp_on);
    in->comp_on = NULL;
    compute_components(in);
    int fon = 0, con = 0, nc = in->component ? in->component->nrows : 0;
    for (int i = 0; i < nf; i++) fon += in->feature_on[i];
    for (int i = 0; i < nc; i++) con += in->comp_on[i];
    logf(in, "Selected %d of %d features, %d of %d components", fon, nf, con, nc);
}

static bool comp_enabled(Inst *in, const char *name)
{
    int c = msidb_find(&in->db, in->component, 0, name, 0);
    return c >= 0 && in->comp_on && in->comp_on[c];
}

static void resolve_directories(Inst *in)
{
    /* every Directory key becomes a property holding its path.  Ones set
     * before costing (command line, Property table, custom actions) or
     * later by MsiSetTargetPath keep their value; the rest are derived
     * from their parents again each time. */
    char b[16], path[MAX_PATH];
    int nd = in->dir ? in->dir->nrows : 0;
    if (!in->dir_explicit) {
        in->dir_explicit = calloc((size_t)nd + 1, 1);
        for (int r = 0; r < nd; r++) in->dir_explicit[r] = prop_set(in, msidb_str(&in->db, in->dir, r, 0, b));
    }
    for (int r = 0; r < nd; r++)
        if (!in->dir_explicit[r]) set_prop(in, msidb_str(&in->db, in->dir, r, 0, b), "");
    for (int r = 0; r < nd; r++) {
        const char *name = msidb_str(&in->db, in->dir, r, 0, b);
        dir_path(in, name, path, sizeof(path));      /* (a set property keeps its value, with a '\') */
        set_prop(in, name, path);
    }
    in->resolved = true;
    if (in->dir) {
        const char *keys[] = { "INSTALLDIR", "INSTALLLOCATION", "APPLICATIONFOLDER", "INSTALLFOLDER" };
        for (int i = 0; i < 4; i++)
            if (prop_set(in, keys[i])) { logf(in, "%s = %s", keys[i], get_prop(in, keys[i])); break; }
    }
}

/* -----------------------------------------------------------------------
 * What the user sees: the progress window, or the package's own progress
 * dialog (its ActionText, ActionData and SetProgress events)
 * ----------------------------------------------------------------------- */
static void ui_detail(Inst *in, const char *text)
{
    if (in->dlg) dlg_event(in->dlg, "ActionData", text, 0, 0);
    else if (in->ui) msiui_status(in->ui, text);
}

static void ui_action_text(Inst *in, const char *text)
{
    if (in->dlg) dlg_event(in->dlg, "ActionText", text, 0, 0);
    else if (in->ui && text[0]) msiui_status(in->ui, text);
}

static void ui_progress(Inst *in, int done, int total)
{
    if (in->dlg) dlg_event(in->dlg, "SetProgress", NULL, done, total);
    else if (in->ui) msiui_progress(in->ui, done, total);
}

static bool ui_cancelled(Inst *in)
{
    if (in->dlg) return dlg_cancelled(in->dlg);
    return in->ui && msiui_cancelled(in->ui);
}

void eng_pump(Inst *in)
{
    if (in->dlg) dlg_pump(in->dlg);
    else if (in->ui) msiui_cancelled(in->ui);
}

/* An action starts: its description from the ActionText table */
static void ui_action(Inst *in, const char *action)
{
    static const struct { const char *a, *d; } std[] = {
        { "InstallFiles", "Copying new files" }, { "RemoveFiles", "Removing files" },
        { "WriteRegistryValues", "Writing system registry values" }, { "RemoveRegistryValues", "Removing system registry values" },
        { "CreateShortcuts", "Creating shortcuts" }, { "RemoveShortcuts", "Removing shortcuts" },
        { "InstallServices", "Installing new services" }, { "StartServices", "Starting services" },
        { "StopServices", "Stopping services" }, { "DeleteServices", "Deleting services" },
        { "WriteEnvironmentStrings", "Updating environment strings" }, { "RegisterProduct", "Registering product" },
        { "CreateFolders", "Creating folders" }, { "RemoveFolders", "Removing folders" },
        { "InstallFinalize", "" }, { "CostFinalize", "Computing space requirements" },
    };
    char text[512] = "", b[16];
    in->action_template[0] = 0;
    MsiTable *t = msidb_table(&in->db, "ActionText");
    int r = t ? msidb_find(&in->db, t, 0, action, 0) : -1;
    if (r >= 0) {
        format_str(in, msidb_str(&in->db, t, r, 1, b), text, sizeof(text));
        snprintf(in->action_template, sizeof(in->action_template), "%s", msidb_str(&in->db, t, r, 2, b));
    } else {
        for (size_t i = 0; i < sizeof(std) / sizeof(std[0]); i++)
            if (!strcmp(std[i].a, action)) snprintf(text, sizeof(text), "%s", std[i].d);
    }
    if (text[0]) ui_action_text(in, text);
}

/* -----------------------------------------------------------------------
 * Rollback
 *
 * While InstallExecuteSequence runs, every change to the machine is noted
 * first in a journal: a file written over or removed is moved to
 * C:\Config.Msi (as Windows keeps .rbf files there), a new file or folder
 * or registry key is remembered, a registry value's old contents are kept,
 * a key the product registration or a service lives in is copied whole,
 * services created, started or stopped are listed, and the package's
 * rollback custom actions are scheduled with their CustomActionData.  If
 * the installation fails or is cancelled before InstallFinalize, the
 * journal is played backwards and the machine is as it was; at
 * InstallFinalize the backups are deleted.  DISABLEROLLBACK=1 or the
 * DisableRollback action turn it off.
 * ----------------------------------------------------------------------- */
typedef void *SC_H;
__declspec(dllimport) SC_H WINAPI OpenSCManagerW(LPCWSTR, LPCWSTR, DWORD);
__declspec(dllimport) SC_H WINAPI OpenServiceW(SC_H, LPCWSTR, DWORD);
__declspec(dllimport) SC_H WINAPI CreateServiceW(SC_H, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD, DWORD, LPCWSTR, LPCWSTR,
                                                 LPDWORD, LPCWSTR, LPCWSTR, LPCWSTR);
__declspec(dllimport) BOOL WINAPI ChangeServiceConfigW(SC_H, DWORD, DWORD, DWORD, LPCWSTR, LPCWSTR, LPDWORD, LPCWSTR,
                                                       LPCWSTR, LPCWSTR, LPCWSTR);
__declspec(dllimport) BOOL WINAPI ChangeServiceConfig2W(SC_H, DWORD, LPVOID);
__declspec(dllimport) BOOL WINAPI StartServiceW(SC_H, DWORD, LPCWSTR *);
__declspec(dllimport) BOOL WINAPI ControlService(SC_H, DWORD, LPVOID);
__declspec(dllimport) BOOL WINAPI DeleteService(SC_H);
__declspec(dllimport) BOOL WINAPI CloseServiceHandle(SC_H);
__declspec(dllimport) BOOL WINAPI QueryServiceStatus(SC_H, LPVOID);

#define SVC_ALL_ACCESS 0xF01FF
#define SCM_ALL_ACCESS 0xF003F
#define SVC_NO_CHANGE  0xFFFFFFFF
#define SERVICES_KEY   L"SYSTEM\\CurrentControlSet\\Services\\"

enum {
    RB_FILE_NEW,          /* a: a file that was not there */
    RB_FILE_SAVED,        /* a: a file replaced or removed; b: its copy in C:\Config.Msi */
    RB_DIR_NEW,           /* a: a folder created */
    RB_DIR_REMOVED,       /* a: a folder removed */
    RB_VALUE,             /* root, a: key, b: value name (NULL: default); old: its contents, if it existed */
    RB_KEY_NEW,           /* root, a: a key created */
    RB_KEY_SAVED,         /* root, a: a key's values, all kept (or none: it did not exist) */
    RB_SERVICE_NEW,       /* a: a service created */
    RB_SERVICE_STARTED,   /* a: a service started */
    RB_SERVICE_STOPPED,   /* a: a service stopped */
    RB_ACTION,            /* a: a rollback custom action; b: its CustomActionData */
};

typedef struct { WCHAR *name; DWORD type, size; BYTE *data; } RbValue;

typedef struct RbOp {
    int      kind;
    HKEY     root;
    char    *a, *b;
    WCHAR   *wa, *wb;
    bool     existed;
    RbValue *vals;
    int      nvals;
} RbOp;

static bool run_custom_action(Inst *in, const char *name);

static WCHAR *wdup(const WCHAR *s)
{
    if (!s) return NULL;
    size_t n = wcslen(s) + 1;
    WCHAR *d = malloc(n * sizeof(WCHAR));
    if (d) memcpy(d, s, n * sizeof(WCHAR));
    return d;
}

static RbOp *rb_add(Inst *in, int kind)
{
    if (!in || !in->rb_on || in->rolling_back) return NULL;
    if (in->nrb == in->rb_cap) {
        int cap = in->rb_cap ? in->rb_cap * 2 : 64;
        RbOp *n = realloc(in->rb, (size_t)cap * sizeof(RbOp));
        if (!n) return NULL;
        in->rb = n;
        in->rb_cap = cap;
    }
    RbOp *o = &in->rb[in->nrb++];
    memset(o, 0, sizeof(*o));
    o->kind = kind;
    return o;
}

static void rb_free_op(RbOp *o)
{
    free(o->a); free(o->b); free(o->wa); free(o->wb);
    for (int i = 0; i < o->nvals; i++) { free(o->vals[i].name); free(o->vals[i].data); }
    free(o->vals);
}

/* A file is about to be written: keep what is there */
static void rb_file(Inst *in, const WCHAR *path)
{
    if (!in || !in->rb_on || in->rolling_back) return;
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        RbOp *o = rb_add(in, RB_FILE_NEW);
        if (o) o->wa = wdup(path);
        return;
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY) return;
    CreateDirectoryW(L"C:\\Config.Msi", NULL);
    WCHAR bak[MAX_PATH];
    _snwprintf(bak, MAX_PATH, L"C:\\Config.Msi\\%08lx%04x.rbf", (unsigned long)GetTickCount(), ++in->rb_serial & 0xFFFF);
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    /* a move when it can (the file is gone after it), else a copy */
    if (!MoveFileExW(path, bak, MOVEFILE_REPLACE_EXISTING) && !CopyFileW(path, bak, FALSE)) {
        logf(in, "Rollback: could not keep a copy of a file (error %lu)", GetLastError());
        return;
    }
    RbOp *o = rb_add(in, RB_FILE_SAVED);
    if (o) { o->wa = wdup(path); o->wb = wdup(bak); }
}

/* Remove a file, keeping it for a rollback */
static bool rb_delete_file(Inst *in, const WCHAR *path)
{
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    if (in && in->rb_on && !in->rolling_back) {
        rb_file(in, path);
        return GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES || DeleteFileW(path);
    }
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(path);
}

static bool rb_remove_dir(Inst *in, const WCHAR *path)
{
    if (!RemoveDirectoryW(path)) return false;
    RbOp *o = rb_add(in, RB_DIR_REMOVED);
    if (o) o->wa = wdup(path);
    return true;
}

static bool make_dirs(Inst *in, const char *path)
{
    /* create every folder on the way (and note the new ones) */
    WCHAR w[MAX_PATH];
    to_w(path, w, MAX_PATH);
    size_t n = wcslen(w);
    for (size_t i = 3; i <= n; i++) {
        if (w[i] != L'\\' && w[i] != 0) continue;
        if (i == n && n && w[n - 1] == L'\\') break;
        WCHAR save = w[i];
        w[i] = 0;
        if (CreateDirectoryW(w, NULL)) { RbOp *o = rb_add(in, RB_DIR_NEW); if (o) o->wa = wdup(w); }
        w[i] = save;
    }
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool read_value(HKEY h, const WCHAR *name, RbValue *v)
{
    DWORD type, size = 0;
    if (RegQueryValueExW(h, name, NULL, &type, NULL, &size)) return false;
    v->data = malloc(size ? size : 1);
    if (!v->data || RegQueryValueExW(h, name, NULL, &type, v->data, &size)) { free(v->data); v->data = NULL; return false; }
    v->type = type;
    v->size = size;
    v->name = wdup(name);
    return true;
}

/* A registry value is about to change (be set or deleted) */
static void rb_value(Inst *in, HKEY root, const WCHAR *key, const WCHAR *name)
{
    RbOp *o = rb_add(in, RB_VALUE);
    if (!o) return;
    o->root = root;
    o->wa = wdup(key);
    o->wb = wdup(name);
    HKEY h;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &h)) return;
    RbValue v;
    memset(&v, 0, sizeof(v));
    if (read_value(h, name, &v)) {
        o->existed = true;
        o->vals = malloc(sizeof(RbValue));
        if (o->vals) { o->vals[0] = v; o->nvals = 1; }
        else { free(v.name); free(v.data); }
    }
    RegCloseKey(h);
}

/* Create a key, noting each key on the way that did not exist */
static LSTATUS rb_create_key(Inst *in, HKEY root, const WCHAR *key, HKEY *out)
{
    WCHAR k[512];
    wcsncpy(k, key, 511);
    k[511] = 0;
    size_t n = wcslen(k);
    for (size_t i = 0; i <= n; i++) {
        if (k[i] != L'\\' && k[i] != 0) continue;
        WCHAR save = k[i];
        k[i] = 0;
        HKEY h;
        if (RegOpenKeyExW(root, k, 0, KEY_READ, &h)) {
            RbOp *o = rb_add(in, RB_KEY_NEW);
            if (o) { o->root = root; o->wa = wdup(k); }
        } else RegCloseKey(h);
        k[i] = save;
    }
    return RegCreateKeyExW(root, key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, out, NULL);
}

/* A whole key is about to be rewritten or deleted: keep its values */
static void rb_key(Inst *in, HKEY root, const WCHAR *key)
{
    RbOp *o = rb_add(in, RB_KEY_SAVED);
    if (!o) return;
    o->root = root;
    o->wa = wdup(key);
    HKEY h;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &h)) return;
    o->existed = true;
    for (DWORD i = 0; ; i++) {
        WCHAR name[256];
        DWORD nl = 256;
        if (RegEnumValueW(h, i, name, &nl, NULL, NULL, NULL, NULL)) break;
        RbValue *nv = realloc(o->vals, (size_t)(o->nvals + 1) * sizeof(RbValue));
        if (!nv) break;
        o->vals = nv;
        memset(&o->vals[o->nvals], 0, sizeof(RbValue));
        if (read_value(h, name, &o->vals[o->nvals])) o->nvals++;
    }
    RegCloseKey(h);
}

static void clear_values(HKEY h)
{
    WCHAR name[256];
    for (int guard = 0; guard < 4096; guard++) {
        DWORD nl = 256;
        if (RegEnumValueW(h, 0, name, &nl, NULL, NULL, NULL, NULL)) break;
        if (RegDeleteValueW(h, name)) break;
    }
}

static void rb_note(Inst *in, int kind, const char *a, const char *b)
{
    RbOp *o = rb_add(in, kind);
    if (!o) return;
    o->a = a ? strdup(a) : NULL;
    o->b = b ? strdup(b) : NULL;
}

static void rb_begin(Inst *in)
{
    in->rb_on = !prop_set(in, "DISABLEROLLBACK");
    if (!in->rb_on) logf(in, "Rollback is disabled (DISABLEROLLBACK)");
}

/* Success (InstallFinalize) or DisableRollback: the backups go */
static void rb_commit(Inst *in)
{
    for (int i = 0; i < in->nrb; i++) {
        if (in->rb[i].kind == RB_FILE_SAVED && in->rb[i].wb) DeleteFileW(in->rb[i].wb);
        rb_free_op(&in->rb[i]);
    }
    free(in->rb);
    in->rb = NULL;
    in->nrb = in->rb_cap = 0;
    in->rb_on = false;
    RemoveDirectoryW(L"C:\\Config.Msi");                    /* (if empty) */
}

static void stop_service(const WCHAR *name, bool remove)
{
    SC_H scm = OpenSCManagerW(NULL, NULL, SCM_ALL_ACCESS);
    SC_H svc = scm ? OpenServiceW(scm, name, SVC_ALL_ACCESS) : NULL;
    if (svc) {
        DWORD st[7];
        if (ControlService(svc, 1 /* SERVICE_CONTROL_STOP */, st))
            for (int i = 0; i < 100 && QueryServiceStatus(svc, st) && st[1] != 1 /* SERVICE_STOPPED */; i++) Sleep(100);
        if (remove) DeleteService(svc);
        CloseServiceHandle(svc);
    }
    if (scm) CloseServiceHandle(scm);
}

/* Failure: undo, newest first */
/* Delete a file during rollback: a program a rollback action just ran may
 * still be mapped for a moment after it exits, so try for a while, then
 * leave it to the next boot */
static bool rb_unlink(Inst *in, const WCHAR *path, const char *u8)
{
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    for (int i = 0; i < 30; i++) {
        DeleteFileW(path);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return true;   /* (gone, not only "deleted") */
        Sleep(100);
    }
    logf(in, "Rollback: %s is in use (error %lu); it goes at the next boot", u8, GetLastError());
    MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    return false;
}

static void rb_run(Inst *in)
{
    if (!in->nrb) { in->rb_on = false; return; }
    logf(in, "Rolling back %d changes", in->nrb);
    ui_action_text(in, "Rolling back action:");
    in->rolling_back = true;
    in->modes[RUNMODE_ROLLBACK] = true;
    int undone = 0;
    for (int i = in->nrb - 1; i >= 0; i--) {
        RbOp *o = &in->rb[i];
        char u8[MAX_PATH * 2] = "";
        if (o->wa) to_u8(o->wa, u8, sizeof(u8));
        switch (o->kind) {
        case RB_FILE_NEW:
            if (GetFileAttributesW(o->wa) != INVALID_FILE_ATTRIBUTES && rb_unlink(in, o->wa, u8)) logf(in, "Rollback: removed %s", u8);
            break;
        case RB_FILE_SAVED:
            if (GetFileAttributesW(o->wa) != INVALID_FILE_ATTRIBUTES && !rb_unlink(in, o->wa, u8)) {
                logf(in, "Rollback: could not restore %s", u8);
                break;
            }
            if (MoveFileExW(o->wb, o->wa, MOVEFILE_REPLACE_EXISTING)) logf(in, "Rollback: restored %s", u8);
            else logf(in, "Rollback: could not restore %s (error %lu)", u8, GetLastError());
            break;
        case RB_DIR_NEW:
            if (RemoveDirectoryW(o->wa)) logf(in, "Rollback: removed folder %s", u8);
            break;
        case RB_DIR_REMOVED:
            CreateDirectoryW(o->wa, NULL);
            break;
        case RB_VALUE: {
            HKEY h;
            if (o->existed && o->nvals) {
                if (!RegCreateKeyExW(o->root, o->wa, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &h, NULL)) {
                    RegSetValueExW(h, o->wb, 0, o->vals[0].type, o->vals[0].data, o->vals[0].size);
                    RegCloseKey(h);
                }
            } else if (!RegOpenKeyExW(o->root, o->wa, 0, KEY_ALL_ACCESS, &h)) {
                RegDeleteValueW(h, o->wb);
                RegCloseKey(h);
            }
            break;
        }
        case RB_KEY_NEW:
            RegDeleteKeyW(o->root, o->wa);
            logf(in, "Rollback: removed registry key %s", u8);
            break;
        case RB_KEY_SAVED: {
            HKEY h;
            if (!o->existed) {
                if (!RegOpenKeyExW(o->root, o->wa, 0, KEY_ALL_ACCESS, &h)) { clear_values(h); RegCloseKey(h); }
                RegDeleteKeyW(o->root, o->wa);
            } else if (!RegCreateKeyExW(o->root, o->wa, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &h, NULL)) {
                clear_values(h);
                for (int k = 0; k < o->nvals; k++)
                    RegSetValueExW(h, o->vals[k].name, 0, o->vals[k].type, o->vals[k].data, o->vals[k].size);
                RegCloseKey(h);
            }
            logf(in, "Rollback: registry key %s %s", u8, o->existed ? "restored" : "removed");
            break;
        }
        case RB_SERVICE_NEW:
        case RB_SERVICE_STARTED: {
            WCHAR w[256];
            to_w(o->a, w, 256);
            stop_service(w, o->kind == RB_SERVICE_NEW);
            logf(in, "Rollback: service %s %s", o->a, o->kind == RB_SERVICE_NEW ? "stopped and deleted" : "stopped");
            break;
        }
        case RB_SERVICE_STOPPED: {
            WCHAR w[256];
            to_w(o->a, w, 256);
            SC_H scm = OpenSCManagerW(NULL, NULL, SCM_ALL_ACCESS);
            SC_H svc = scm ? OpenServiceW(scm, w, SVC_ALL_ACCESS) : NULL;
            if (svc) { StartServiceW(svc, 0, NULL); CloseServiceHandle(svc); }
            if (scm) CloseServiceHandle(scm);
            logf(in, "Rollback: service %s started again", o->a);
            break;
        }
        case RB_ACTION: {
            char *saved = strdup(get_prop(in, o->a));
            set_prop(in, o->a, o->b ? o->b : "");
            logf(in, "Rollback: custom action %s", o->a);
            run_custom_action(in, o->a);
            set_prop(in, o->a, saved);
            free(saved);
            break;
        }
        }
        undone++;
        eng_pump(in);
    }
    in->rolling_back = false;
    in->modes[RUNMODE_ROLLBACK] = false;
    for (int i = 0; i < in->nrb; i++) rb_free_op(&in->rb[i]);
    free(in->rb);
    in->rb = NULL;
    in->nrb = in->rb_cap = 0;
    in->rb_on = false;
    RemoveDirectoryW(L"C:\\Config.Msi");
    logf(in, "Rollback complete: %d changes undone", undone);
}

/* -----------------------------------------------------------------------
 * Files
 * ----------------------------------------------------------------------- */
typedef struct {
    int      row;                    /* File table row */
    char     key[80];
    char     target[MAX_PATH];
    uint32_t size;
    int      sequence;
    bool     done;
} PlanFile;

static int cmp_seq(const void *a, const void *b)
{
    return ((const PlanFile *)a)->sequence - ((const PlanFile *)b)->sequence;
}

/* The files to install, in sequence order */
static PlanFile *plan_files(Inst *in, int *count)
{
    MsiTable *ft = in->file;
    int n = ft ? ft->nrows : 0;
    PlanFile *pf = calloc((size_t)n + 1, sizeof(PlanFile));
    int k = 0;
    char b[16];
    for (int r = 0; r < n; r++) {
        if (!comp_enabled(in, msidb_str(&in->db, ft, r, 1, b))) continue;
        PlanFile *f = &pf[k];
        f->row = r;
        snprintf(f->key, sizeof(f->key), "%s", msidb_str(&in->db, ft, r, 0, b));
        file_path(in, f->key, f->target, sizeof(f->target));
        f->size = (uint32_t)msidb_int(&in->db, ft, r, msidb_col(ft, "FileSize"), NULL);
        f->sequence = msidb_int(&in->db, ft, r, msidb_col(ft, "Sequence"), NULL);
        k++;
    }
    qsort(pf, (size_t)k, sizeof(PlanFile), cmp_seq);
    *count = k;
    return pf;
}

typedef struct {
    Cab       cab;
    void     *data;                  /* stream or file contents */
    size_t    size;
    char      name[256];
} OpenCab;

/* Load the cabinet of Media row @m: an embedded stream ("#name") or a
 * file next to the package */
static bool load_cab(Inst *in, int m, OpenCab *oc)
{
    char b[16];
    memset(oc, 0, sizeof(*oc));
    const char *cabname = msidb_str(&in->db, in->media, m, msidb_col(in->media, "Cabinet"), b);
    snprintf(oc->name, sizeof(oc->name), "%s", cabname);
    if (!*cabname) { fail(in, MSI_ERROR_PACKAGE_INVALID, "Media %d has no cabinet", m); return false; }
    if (cabname[0] == '#') {
        oc->data = msidb_read_stream(&in->db, cabname + 1, &oc->size);
        if (!oc->data) { fail(in, MSI_ERROR_PACKAGE_INVALID, "Cabinet stream %s is missing", cabname + 1); return false; }
    } else {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s%s", in->source_dir, cabname);
        WCHAR w[MAX_PATH];
        to_w(path, w, MAX_PATH);
        HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { fail(in, MSI_ERROR_PACKAGE_OPEN, "Cabinet %s not found beside the package", cabname); return false; }
        DWORD sz = GetFileSize(h, NULL), rd = 0;
        oc->data = malloc(sz ? sz : 1);
        if (!oc->data || !ReadFile(h, oc->data, sz, &rd, NULL) || rd != sz) {
            CloseHandle(h);
            fail(in, MSI_ERROR_PACKAGE_OPEN, "Could not read cabinet %s", cabname);
            return false;
        }
        CloseHandle(h);
        oc->size = sz;
    }
    if (!cab_open(&oc->cab, oc->data, oc->size)) { fail(in, MSI_ERROR_PACKAGE_INVALID, "%s is not a cabinet", cabname); return false; }
    return true;
}

static void free_cab(OpenCab *oc)
{
    cab_close(&oc->cab);
    free(oc->data);
    memset(oc, 0, sizeof(*oc));
}

static PlanFile *plan_by_key(PlanFile *pf, int n, const char *key)
{
    for (int i = 0; i < n; i++) if (!strcmp(pf[i].key, key)) return &pf[i];
    return NULL;
}

/* An output file being written from a folder's stream */
typedef struct {
    PlanFile *pf;
    uint32_t  start, end;            /* folder offsets */
    HANDLE    h;
    uint32_t  written;
} Writer;

static bool open_writer(Inst *in, Writer *w)
{
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", w->pf->target);
    char *bs = strrchr(dir, '\\');
    if (bs) { bs[1] = 0; make_dirs(in, dir); }
    WCHAR wp[MAX_PATH];
    to_w(w->pf->target, wp, MAX_PATH);
    SetFileAttributesW(wp, FILE_ATTRIBUTE_NORMAL);
    rb_file(in, wp);
    w->h = CreateFileW(wp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (w->h == INVALID_HANDLE_VALUE) {
        fail(in, MSI_ERROR_FAILURE, "Could not create %s (error %lu)", w->pf->target, GetLastError());
        return false;
    }
    if (in->ui || in->dlg) {
        char status[MAX_PATH + 32];
        const char *name = strrchr(w->pf->target, '\\');
        snprintf(status, sizeof(status), in->dlg ? "%s" : "Copying new files: %s", name ? name + 1 : w->pf->target);
        ui_detail(in, status);
    }
    logf(in, "Installing %s (%u bytes)", w->pf->target, w->pf->size);
    return true;
}

static void close_writer(Inst *in, Writer *w)
{
    if (w->h && w->h != INVALID_HANDLE_VALUE) CloseHandle(w->h);
    w->h = NULL;
    w->pf->done = true;
    in->done_work++;
    ui_progress(in, in->done_work, in->total_work);
}

/* Extract one cabinet set (starting at Media row @m) for the planned
 * files it holds; continues into following cabinets when a folder spans */
/* The Media row a file's sequence number puts it on: the one with the
 * smallest LastSequence at or above it (a patch's files, at 10000 and
 * up, come from its own cabinet even when the product's has an entry
 * with the same key) */
static int media_of(Inst *in, int sequence)
{
    int col = msidb_col(in->media, "LastSequence"), best = -1, best_last = 0;
    for (int m = 0; in->media && m < in->media->nrows; m++) {
        int last = msidb_int(&in->db, in->media, m, col, NULL);
        if (last >= sequence && (best < 0 || last < best_last)) { best = m; best_last = last; }
    }
    return best;
}

static bool extract_media(Inst *in, int m, PlanFile *pf, int npf)
{
    OpenCab oc;
    if (!load_cab(in, m, &oc)) return false;
    bool ok = true;
    for (int fo = 0; ok && fo < oc.cab.nfolders; fo++) {
        /* the files of this folder, in offset order as listed */
        Writer *ws = calloc((size_t)oc.cab.nfiles + 1, sizeof(Writer));
        int nw = 0;
        bool spans = false;
        for (int i = 0; i < oc.cab.nfiles; i++) {
            CabFile *cf = &oc.cab.files[i];
            if (cf->folder != fo) continue;
            if (cf->continued_next) spans = true;
            PlanFile *p = plan_by_key(pf, npf, cf->name);
            if (!p || p->done || media_of(in, p->sequence) != m) continue;
            ws[nw].pf = p;
            ws[nw].start = cf->folder_off;
            ws[nw].end = cf->folder_off + cf->size;
            nw++;
        }
        if (!nw && !spans) { free(ws); continue; }
        CabReader r;
        if (!cab_reader_start(&r, &oc.cab, fo)) { fail(in, MSI_ERROR_PACKAGE_INVALID, "%s", r.error); free(ws); ok = false; break; }
        uint32_t pos = 0;
        OpenCab next;
        memset(&next, 0, sizeof(next));
        int media = m;
        for (;;) {
            if (!cab_reader_next(&r)) {
                if (r.error[0]) { fail(in, MSI_ERROR_PACKAGE_INVALID, "%s: %s", oc.name, r.error); ok = false; break; }
                /* end of this cabinet's part of the folder */
                bool more = false;
                for (int i = 0; i < nw; i++) if (ws[i].written < ws[i].pf->size) more = true;
                if (!more) break;
                /* the folder continues in the next cabinet */
                if (next.data) free_cab(&next);
                media++;
                if (media >= in->media->nrows) { fail(in, MSI_ERROR_PACKAGE_INVALID, "Folder continues past the last cabinet"); ok = false; break; }
                if (!load_cab(in, media, &next)) { ok = false; break; }
                if (!cab_reader_continue(&r, &next.cab)) { fail(in, MSI_ERROR_PACKAGE_INVALID, "%s", r.error); ok = false; break; }
                continue;
            }
            uint32_t bstart = pos, bend = pos + r.out_len;
            for (int i = 0; i < nw; i++) {
                Writer *w = &ws[i];
                if (w->end <= bstart || w->start >= bend) continue;
                uint32_t s = w->start > bstart ? w->start : bstart;
                uint32_t e = w->end < bend ? w->end : bend;
                if (!w->h) { if (!open_writer(in, w)) { ok = false; break; } }
                DWORD wr = 0;
                if (!WriteFile(w->h, r.out + (s - bstart), e - s, &wr, NULL) || wr != e - s) {
                    fail(in, MSI_ERROR_FAILURE, "Could not write %s", w->pf->target);
                    ok = false;
                    break;
                }
                w->written += e - s;
                if (w->written >= w->pf->size) close_writer(in, w);
            }
            if (!ok) break;
            pos = bend;
            if (ui_cancelled(in)) { fail(in, MSI_ERROR_USEREXIT, "Cancelled by the user"); ok = false; break; }
        }
        for (int i = 0; i < nw; i++) if (ws[i].h) close_writer(in, &ws[i]);
        cab_reader_end(&r);
        if (next.data) free_cab(&next);
        free(ws);
    }
    free_cab(&oc);
    return ok;
}

static bool copy_uncompressed(Inst *in, PlanFile *p)
{
    /* a file shipped beside the package rather than in a cabinet */
    char b[16], name[MAX_PATH], src[MAX_PATH];
    long_name(msidb_str(&in->db, in->file, p->row, 2, b), name, sizeof(name));
    snprintf(src, sizeof(src), "%s%s", in->source_dir, name);
    WCHAR ws[MAX_PATH], wd[MAX_PATH];
    to_w(src, ws, MAX_PATH);
    to_w(p->target, wd, MAX_PATH);
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", p->target);
    char *bs = strrchr(dir, '\\');
    if (bs) { bs[1] = 0; make_dirs(in, dir); }
    logf(in, "Copying %s to %s", src, p->target);
    if (GetFileAttributesW(ws) != INVALID_FILE_ATTRIBUTES) rb_file(in, wd);
    if (!CopyFileW(ws, wd, FALSE)) { fail(in, MSI_ERROR_PACKAGE_OPEN, "Source file %s is missing", src); return false; }
    p->done = true;
    in->done_work++;
    ui_progress(in, in->done_work, in->total_work);
    return true;
}

static bool action_install_files(Inst *in)
{
    int n;
    PlanFile *pf = plan_files(in, &n);
    in->total_work = n + 1;
    ui_progress(in, 0, in->total_work);
    bool ok = true;
    char b[16];
    int attr_col = msidb_col(in->file, "Attributes");
    int lastseq_col = msidb_col(in->media, "LastSequence");
    /* files marked uncompressed come from the source folder */
    for (int i = 0; ok && i < n; i++) {
        int attrs = msidb_int(&in->db, in->file, pf[i].row, attr_col, NULL);
        if (attrs & 0x2000) ok = copy_uncompressed(in, &pf[i]);
    }
    /* the rest from the cabinets, in Media order */
    for (int m = 0; ok && in->media && m < in->media->nrows; m++) {
        int last = msidb_int(&in->db, in->media, m, lastseq_col, NULL);
        bool needed = false;
        for (int i = 0; i < n; i++) if (!pf[i].done && pf[i].sequence <= last) needed = true;
        if (!needed) continue;
        const char *cab = msidb_str(&in->db, in->media, m, msidb_col(in->media, "Cabinet"), b);
        if (!*cab) {                             /* uncompressed media */
            for (int i = 0; ok && i < n; i++)
                if (!pf[i].done && pf[i].sequence <= last) ok = copy_uncompressed(in, &pf[i]);
            continue;
        }
        ok = extract_media(in, m, pf, n);
    }
    for (int i = 0; ok && i < n; i++) {
        if (pf[i].done) continue;
        if (pf[i].size == 0) {                   /* empty files are often left out of the cabinet */
            char dir[MAX_PATH];
            snprintf(dir, sizeof(dir), "%s", pf[i].target);
            char *slash = strrchr(dir, '\\');
            if (slash) { slash[1] = 0; make_dirs(in, dir); }
            WCHAR wt[MAX_PATH];
            to_w(pf[i].target, wt, MAX_PATH);
            rb_file(in, wt);
            HANDLE h = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); pf[i].done = true; continue; }
        }
        logf(in, "Warning: %s was not in any cabinet", pf[i].key);
    }
    free(pf);
    return ok;
}

static bool action_remove_files(Inst *in)
{
    int n;
    PlanFile *pf = plan_files(in, &n);
    in->total_work = n + 1;
    for (int i = 0; i < n; i++) {
        WCHAR w[MAX_PATH];
        to_w(pf[i].target, w, MAX_PATH);
        if (rb_delete_file(in, w)) logf(in, "Removed %s", pf[i].target);
        else if (GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES) logf(in, "Could not remove %s", pf[i].target);
        in->done_work++;
        ui_progress(in, in->done_work, in->total_work);
    }
    free(pf);
    return true;
}

static void action_create_folders(Inst *in)
{
    MsiTable *t = msidb_table(&in->db, "CreateFolder");
    char b[16], path[MAX_PATH];
    for (int r = 0; t && r < t->nrows; r++) {
        if (!comp_enabled(in, msidb_str(&in->db, t, r, 1, b))) continue;
        dir_path(in, msidb_str(&in->db, t, r, 0, b), path, sizeof(path));
        make_dirs(in, path);
        logf(in, "Created folder %s", path);
    }
}

/* Remove a folder and its now-empty parents, stopping at the standard
 * folders (Programs, Windows, ...) */
static void remove_folder_chain(Inst *in, const char *path)
{
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s", path);
    for (int depth = 0; depth < 8; depth++) {
        size_t n = strlen(p);
        while (n && p[n - 1] == '\\') p[--n] = 0;
        if (n <= 3) return;
        if (!_stricmp(p, "C:\\Programs") || !_stricmp(p, "C:\\Windows") || !_stricmp(p, "C:\\Windows\\System32") ||
            !_stricmp(p, "C:\\ProgramData") || !_stricmp(p, "C:\\AppData\\Roaming") || !_stricmp(p, "C:\\AppData\\Local"))
            return;
        WCHAR w[MAX_PATH];
        to_w(p, w, MAX_PATH);
        if (!rb_remove_dir(in, w)) return;
        logf(in, "Removed folder %s", p);
        char *bs = strrchr(p, '\\');
        if (!bs) return;
        *bs = 0;
    }
}

static void action_remove_folders(Inst *in)
{
    char b[16], path[MAX_PATH];
    MsiTable *t = msidb_table(&in->db, "CreateFolder");
    for (int r = 0; t && r < t->nrows; r++) {
        if (!comp_enabled(in, msidb_str(&in->db, t, r, 1, b))) continue;
        dir_path(in, msidb_str(&in->db, t, r, 0, b), path, sizeof(path));
        remove_folder_chain(in, path);
    }
    for (int c = 0; in->component && c < in->component->nrows; c++) {
        if (!in->comp_on[c]) continue;
        dir_path(in, msidb_str(&in->db, in->component, c, 2, b), path, sizeof(path));
        remove_folder_chain(in, path);
    }
}

/* -----------------------------------------------------------------------
 * Registry table
 * ----------------------------------------------------------------------- */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void write_registry_row(Inst *in, MsiTable *t, int r)
{
    char b[16];
    int root = msidb_int(&in->db, t, r, 1, NULL);
    char key[512], name[256], value[2048];
    format_str(in, msidb_str(&in->db, t, r, 2, b), key, sizeof(key));
    format_str(in, msidb_str(&in->db, t, r, 3, b), name, sizeof(name));
    const char *raw = msidb_str(&in->db, t, r, 4, b);
    WCHAR wkey[512], wname[256];
    to_w(key, wkey, 512);
    to_w(name, wname, 256);
    HKEY h;
    if (rb_create_key(in, root_key(in, root), wkey, &h)) {
        logf(in, "Could not create registry key %s", key);
        return;
    }
    if (!strcmp(name, "*") || !strcmp(name, "+") || !strcmp(name, "-")) { RegCloseKey(h); return; }
    const WCHAR *vname = name[0] ? wname : NULL;
    rb_value(in, root_key(in, root), wkey, vname);
    if (raw[0] == '#' && raw[1] == 'x') {                          /* REG_BINARY */
        BYTE bin[1024];
        int n = 0;
        for (const char *p = raw + 2; p[0] && p[1] && n < (int)sizeof(bin); p += 2) {
            int a = hexval(p[0]), c = hexval(p[1]);
            if (a < 0 || c < 0) break;
            bin[n++] = (BYTE)(a * 16 + c);
        }
        RegSetValueExW(h, vname, 0, REG_BINARY, bin, (DWORD)n);
    } else if (raw[0] == '#' && raw[1] == '%') {                   /* REG_EXPAND_SZ */
        format_str(in, raw + 2, value, sizeof(value));
        WCHAR wv[2048];
        to_w(value, wv, 2048);
        RegSetValueExW(h, vname, 0, REG_EXPAND_SZ, (const BYTE *)wv, (DWORD)(wcslen(wv) + 1) * 2);
    } else if (raw[0] == '#' && raw[1] != '#') {                   /* REG_DWORD */
        format_str(in, raw + 1, value, sizeof(value));
        DWORD d = (DWORD)strtoul(value, NULL, 10);
        RegSetValueExW(h, vname, 0, REG_DWORD, (const BYTE *)&d, 4);
    } else if (strstr(raw, "[~]")) {                               /* REG_MULTI_SZ */
        /* pieces separated by [~]; leading/trailing [~] mean append/prepend
         * to an existing value: treated as a plain set here */
        WCHAR multi[2048];
        int n = 0;
        const char *p = raw;
        while (*p) {
            const char *e = strstr(p, "[~]");
            char piece[512];
            size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l >= sizeof(piece)) l = sizeof(piece) - 1;
            memcpy(piece, p, l);
            piece[l] = 0;
            if (l) {
                format_str(in, piece, value, sizeof(value));
                WCHAR wv[512];
                to_w(value, wv, 512);
                for (int i = 0; wv[i] && n < 2046; i++) multi[n++] = wv[i];
                multi[n++] = 0;
            }
            if (!e) break;
            p = e + 3;
        }
        multi[n++] = 0;
        RegSetValueExW(h, vname, 0, REG_MULTI_SZ, (const BYTE *)multi, (DWORD)n * 2);
    } else {                                                       /* REG_SZ */
        format_str(in, raw[0] == '#' ? raw + 1 : raw, value, sizeof(value));
        WCHAR wv[2048];
        to_w(value, wv, 2048);
        RegSetValueExW(h, vname, 0, REG_SZ, (const BYTE *)wv, (DWORD)(wcslen(wv) + 1) * 2);
    }
    RegCloseKey(h);
}

static void action_write_registry(Inst *in)
{
    MsiTable *t = msidb_table(&in->db, "Registry");
    char b[16];
    int n = 0;
    for (int r = 0; t && r < t->nrows; r++) {
        if (!comp_enabled(in, msidb_str(&in->db, t, r, 5, b))) continue;
        write_registry_row(in, t, r);
        n++;
    }
    if (n) logf(in, "Wrote %d registry values", n);
}

static void action_remove_registry(Inst *in)
{
    MsiTable *t = msidb_table(&in->db, "Registry");
    char b[16];
    for (int r = 0; t && r < t->nrows; r++) {
        if (!comp_enabled(in, msidb_str(&in->db, t, r, 5, b))) continue;
        int root = msidb_int(&in->db, t, r, 1, NULL);
        char key[512], name[256];
        format_str(in, msidb_str(&in->db, t, r, 2, b), key, sizeof(key));
        format_str(in, msidb_str(&in->db, t, r, 3, b), name, sizeof(name));
        WCHAR wkey[512], wname[256];
        to_w(key, wkey, 512);
        to_w(name, wname, 256);
        if (!strcmp(name, "*") || !strcmp(name, "-")) { rb_key(in, root_key(in, root), wkey); RegDeleteKeyW(root_key(in, root), wkey); continue; }
        if (!strcmp(name, "+")) continue;
        HKEY h;
        if (RegOpenKeyExW(root_key(in, root), wkey, 0, KEY_ALL_ACCESS, &h)) continue;
        rb_value(in, root_key(in, root), wkey, name[0] ? wname : NULL);
        RegDeleteValueW(h, name[0] ? wname : NULL);
        RegCloseKey(h);
        RegDeleteKeyW(root_key(in, root), wkey);   /* goes only if empty */
    }
}

/* -----------------------------------------------------------------------
 * Environment table: variables in the registry (the system's with '*',
 * else the user's), which new programs start with.  A Name is prefixed by
 * flags ('=' and '+' set on install, '-' removes on uninstall, '!' removes
 * on install, '*' system); "[~]" in a Value stands for the current value,
 * so "[~];X" appends X and "X;[~]" prepends it.
 * ----------------------------------------------------------------------- */
#define ENV_SYS_KEY  L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"
#define ENV_USER_KEY L"Environment"

typedef struct { bool sys, set, remove_on_uninstall, remove_on_install; char name[128]; } EnvFlags;

static void env_flags(const char *raw, EnvFlags *f)
{
    memset(f, 0, sizeof(*f));
    for (; *raw == '=' || *raw == '+' || *raw == '-' || *raw == '!' || *raw == '*'; raw++) {
        if (*raw == '*') f->sys = true;
        else if (*raw == '-') f->remove_on_uninstall = true;
        else if (*raw == '!') f->remove_on_install = true;
        else f->set = true;
    }
    snprintf(f->name, sizeof(f->name), "%s", raw);
}

static HKEY env_key(bool sys, bool create)
{
    HKEY h = 0;
    if (create) RegCreateKeyExW(sys ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, sys ? ENV_SYS_KEY : ENV_USER_KEY,
                                0, 0, 0, KEY_ALL_ACCESS, 0, &h, 0);
    else RegOpenKeyExW(sys ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, sys ? ENV_SYS_KEY : ENV_USER_KEY, 0, KEY_ALL_ACCESS, &h);
    return h;
}

static void env_read(HKEY h, const WCHAR *name, char *out, int cap)
{
    WCHAR w[2048];
    DWORD n = sizeof(w) - 2, type = 0;
    out[0] = 0;
    if (RegQueryValueExW(h, name, 0, &type, (BYTE *)w, &n) || (type != REG_SZ && type != REG_EXPAND_SZ)) return;
    w[n / 2] = 0;
    to_u8(w, out, cap);
}

/* Splits a Value into what goes before and after the current one: returns
 * false when there is no "[~]" (the value replaces the variable) */
static bool env_parts(Inst *in, const char *raw, char *pre, char *post, int cap)
{
    const char *t = strstr(raw, "[~]");
    char a[2048], b[2048];
    if (!t) { format_str(in, raw, pre, cap); post[0] = 0; return false; }
    snprintf(a, sizeof(a), "%.*s", (int)(t - raw), raw);
    snprintf(b, sizeof(b), "%s", t + 3);
    format_str(in, a, pre, cap);
    format_str(in, b, post, cap);
    return true;
}

static void action_write_env(Inst *in)
{
    MsiTable *t = msidb_table(&in->db, "Environment");
    char b[16];
    int n = 0;
    for (int r = 0; t && r < t->nrows; r++) {
        if (!comp_enabled(in, msidb_str(&in->db, t, r, 3, b))) continue;
        EnvFlags f;
        env_flags(msidb_str(&in->db, t, r, 1, b), &f);
        const char *raw = msidb_str(&in->db, t, r, 2, b);
        WCHAR wname[128];
        to_w(f.name, wname, 128);
        HKEY h = env_key(f.sys, true);
        if (!h) continue;
        rb_value(in, f.sys ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, f.sys ? ENV_SYS_KEY : ENV_USER_KEY, wname);
        if (f.remove_on_install) { RegDeleteValueW(h, wname); RegCloseKey(h); n++; continue; }
        char cur[2048], pre[2048], post[2048], val[4096];
        env_read(h, wname, cur, sizeof(cur));
        if (env_parts(in, raw, pre, post, sizeof(pre))) {
            if (!cur[0]) {                                     /* nothing to add to: drop the separator */
                const char *p = pre, *q = post;
                if (*q == ';') q++;
                size_t pl = strlen(p);
                snprintf(val, sizeof(val), "%.*s%s", (int)(pl && p[pl - 1] == ';' ? pl - 1 : pl), p, q);
            } else if ((pre[0] && strstr(cur, pre)) || (post[0] && strstr(cur, post[0] == ';' ? post + 1 : post))) {
                snprintf(val, sizeof(val), "%s", cur);          /* already there */
            } else snprintf(val, sizeof(val), "%s%s%s", pre, cur, post);
        } else snprintf(val, sizeof(val), "%s", pre);
        WCHAR wv[4096];
        to_w(val, wv, 4096);
        DWORD type = strchr(val, '%') ? REG_EXPAND_SZ : REG_SZ;
        RegSetValueExW(h, wname, 0, type, (const BYTE *)wv, (DWORD)(wcslen(wv) + 1) * 2);
        RegCloseKey(h);
        logf(in, "Environment: %s%s = %s", f.sys ? "(system) " : "", f.name, val);
        n++;
    }
    if (n) logf(in, "Wrote %d environment variables (new programs see them)", n);
}

/* Uninstall: what was appended or prepended comes out again; a variable the
 * package set (or one marked '-') goes */
static void action_remove_env(Inst *in)
{
    MsiTable *t = msidb_table(&in->db, "Environment");
    char b[16];
    for (int r = 0; t && r < t->nrows; r++) {
        if (!comp_enabled(in, msidb_str(&in->db, t, r, 3, b))) continue;
        EnvFlags f;
        env_flags(msidb_str(&in->db, t, r, 1, b), &f);
        if (f.remove_on_install) continue;
        const char *raw = msidb_str(&in->db, t, r, 2, b);
        WCHAR wname[128];
        to_w(f.name, wname, 128);
        HKEY h = env_key(f.sys, false);
        if (!h) continue;
        char cur[2048], pre[2048], post[2048];
        env_read(h, wname, cur, sizeof(cur));
        rb_value(in, f.sys ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, f.sys ? ENV_SYS_KEY : ENV_USER_KEY, wname);
        if (env_parts(in, raw, pre, post, sizeof(pre)) && !f.remove_on_uninstall) {
            char out[2048];
            const char *piece = pre[0] ? pre : post;
            char *at = piece[0] ? strstr(cur, piece) : 0;
            if (!at) { RegCloseKey(h); continue; }
            snprintf(out, sizeof(out), "%.*s%s", (int)(at - cur), cur, at + strlen(piece));
            if (!out[0]) RegDeleteValueW(h, wname);
            else {
                WCHAR wv[2048];
                to_w(out, wv, 2048);
                RegSetValueExW(h, wname, 0, strchr(out, '%') ? REG_EXPAND_SZ : REG_SZ, (const BYTE *)wv, (DWORD)(wcslen(wv) + 1) * 2);
            }
        } else RegDeleteValueW(h, wname);
        RegCloseKey(h);
    }
}

/* -----------------------------------------------------------------------
 * Product registration (Add/Remove Programs) and the cached package
 * ----------------------------------------------------------------------- */
static void set_sz(HKEY h, const WCHAR *name, const char *u8)
{
    WCHAR w[1024];
    to_w(u8, w, 1024);
    RegSetValueExW(h, name, 0, REG_SZ, (const BYTE *)w, (DWORD)(wcslen(w) + 1) * 2);
}

static void set_dw(HKEY h, const WCHAR *name, DWORD v) { RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE *)&v, 4); }

static void install_location(Inst *in, char *out, int cap)
{
    for (size_t i = 0; i < sizeof(g_install_keys) / sizeof(g_install_keys[0]); i++)
        if (prop_set(in, g_install_keys[i])) { snprintf(out, (size_t)cap, "%s", get_prop(in, g_install_keys[i])); return; }
    snprintf(out, (size_t)cap, "%s", get_prop(in, "TARGETDIR"));
}

static void action_register_product(Inst *in)
{
    const char *code = get_prop(in, "ProductCode");
    if (!*code) return;
    WCHAR wc[64], key[300];
    to_w(code, wc, 64);
    _snwprintf(key, 300, L"%s\\%s", UNINSTALL_KEY, wc);
    HKEY h;
    rb_key(in, HKEY_LOCAL_MACHINE, key);
    if (!RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &h, NULL)) {
        set_sz(h, L"DisplayName", get_prop(in, "ProductName"));
        set_sz(h, L"DisplayVersion", get_prop(in, "ProductVersion"));
        set_sz(h, L"Publisher", get_prop(in, "Manufacturer"));
        char loc[MAX_PATH];
        install_location(in, loc, sizeof(loc));
        set_sz(h, L"InstallLocation", loc);
        set_sz(h, L"InstallSource", in->source_dir);
        char s[128];
        snprintf(s, sizeof(s), "MsiExec.exe /X%s", code);
        set_sz(h, L"UninstallString", s);
        snprintf(s, sizeof(s), "MsiExec.exe /I%s", code);
        set_sz(h, L"ModifyPath", s);
        if (prop_set(in, "ARPHELPLINK")) { char v[512]; format_str(in, get_prop(in, "ARPHELPLINK"), v, sizeof(v)); set_sz(h, L"HelpLink", v); }
        if (prop_set(in, "ARPURLINFOABOUT")) set_sz(h, L"URLInfoAbout", get_prop(in, "ARPURLINFOABOUT"));
        if (prop_set(in, "ARPURLUPDATEINFO")) set_sz(h, L"URLUpdateInfo", get_prop(in, "ARPURLUPDATEINFO"));
        if (prop_set(in, "ARPCONTACT")) set_sz(h, L"Contact", get_prop(in, "ARPCONTACT"));
        if (prop_set(in, "ARPCOMMENTS")) set_sz(h, L"Comments", get_prop(in, "ARPCOMMENTS"));
        if (prop_set(in, "ARPNOMODIFY")) set_dw(h, L"NoModify", 1);
        if (prop_set(in, "ARPNOREPAIR")) set_dw(h, L"NoRepair", 1);
        if (prop_set(in, "ARPSYSTEMCOMPONENT")) set_dw(h, L"SystemComponent", 1);
        set_dw(h, L"WindowsInstaller", 1);
        set_dw(h, L"Language", (DWORD)atoi(get_prop(in, "ProductLanguage")));
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(s, sizeof(s), "%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
        set_sz(h, L"InstallDate", s);
        /* version as Windows stores it: major.minor.build packed */
        unsigned maj = 0, min = 0, bld = 0;
        sscanf(get_prop(in, "ProductVersion"), "%u.%u.%u", &maj, &min, &bld);
        set_dw(h, L"Version", (maj << 24) | (min << 16) | (bld & 0xFFFF));
        set_dw(h, L"VersionMajor", maj);
        set_dw(h, L"VersionMinor", min);
        unsigned long long bytes = 0;
        for (int r = 0; in->file && r < in->file->nrows; r++)
            if (comp_enabled(in, msidb_str(&in->db, in->file, r, 1, s)))
                bytes += (unsigned)msidb_int(&in->db, in->file, r, msidb_col(in->file, "FileSize"), NULL);
        set_dw(h, L"EstimatedSize", (DWORD)(bytes / 1024));
        RegCloseKey(h);
    }
    /* the cached package, for uninstalling */
    make_dirs(in, "C:\\Windows\\Installer\\");
    WCHAR cache[MAX_PATH];
    _snwprintf(cache, MAX_PATH, L"C:\\Windows\\Installer\\%s.msi", wc);
    if (_wcsicmp(in->pkg_path, cache)) {                     /* (a repair runs from the cached copy) */
        rb_file(in, cache);
        if (!CopyFileW(in->pkg_path, cache, FALSE)) logf(in, "Could not cache the package");
    }
    _snwprintf(key, 300, L"%s\\%s", NOVA_INSTALLER_KEY, wc);
    rb_key(in, HKEY_LOCAL_MACHINE, key);
    if (!rb_create_key(in, HKEY_LOCAL_MACHINE, key, &h)) {
        RegSetValueExW(h, L"LocalPackage", 0, REG_SZ, (const BYTE *)cache, (DWORD)(wcslen(cache) + 1) * 2);
        set_sz(h, L"ProductName", get_prop(in, "ProductName"));
        set_sz(h, L"UpgradeCode", get_prop(in, "UpgradeCode"));
        set_sz(h, L"ProductVersion", get_prop(in, "ProductVersion"));
        char loc[MAX_PATH];
        install_location(in, loc, sizeof(loc));
        set_sz(h, L"InstallLocation", loc);
        /* the features installed, for maintenance and removal */
        char feats[4096] = ",", b[16];
        for (int i = 0; in->feature && i < in->feature->nrows; i++) {
            if (!in->feature_on || !in->feature_on[i]) continue;
            size_t n = strlen(feats);
            snprintf(feats + n, sizeof(feats) - n, "%s,", msidb_str(&in->db, in->feature, i, 0, b));
        }
        set_sz(h, L"Features", feats);
        cache_changes(in, h);
        RegCloseKey(h);
    }
    logf(in, "Registered product %s (%s)", get_prop(in, "ProductName"), code);
}

static void action_unregister_product(Inst *in)
{
    const char *code = get_prop(in, "ProductCode");
    if (!*code) return;
    WCHAR wc[64], key[300];
    to_w(code, wc, 64);
    _snwprintf(key, 300, L"%s\\%s", UNINSTALL_KEY, wc);
    rb_key(in, HKEY_LOCAL_MACHINE, key);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    _snwprintf(key, 300, L"%s\\%s", NOVA_INSTALLER_KEY, wc);
    rb_key(in, HKEY_LOCAL_MACHINE, key);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    WCHAR cache[MAX_PATH];
    _snwprintf(cache, MAX_PATH, L"C:\\Windows\\Installer\\%s.msi", wc);
    rb_delete_file(in, cache);
    /* the copies of its transforms and patches */
    WCHAR dir[MAX_PATH], pat[MAX_PATH];
    _snwprintf(dir, MAX_PATH, L"C:\\Windows\\Installer\\%s", wc);
    _snwprintf(pat, MAX_PATH, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            WCHAR p[MAX_PATH];
            _snwprintf(p, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            rb_delete_file(in, p);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    rb_remove_dir(in, dir);
    logf(in, "Unregistered product %s", code);
}

/* -----------------------------------------------------------------------
 * Other standard actions
 * ----------------------------------------------------------------------- */
static bool action_launch_conditions(Inst *in)
{
    MsiTable *t = msidb_table(&in->db, "LaunchCondition");
    char b[16];
    for (int r = 0; t && r < t->nrows; r++) {
        if (cond(in, msidb_str(&in->db, t, r, 0, b))) continue;
        char msg[512];
        format_str(in, msidb_str(&in->db, t, r, 1, b), msg, sizeof(msg));
        fail(in, MSI_ERROR_FAILURE, "%s", msg[0] ? msg : "A launch condition of the package failed");
        return false;
    }
    return true;
}

/* A file's version from its VS_FIXEDFILEINFO (the 0xFEEF04BD signature in
 * its version resource); false when it has none */
static bool file_version(const WCHAR *path, unsigned v[4])
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    static uint8_t buf[65536 + 16];
    size_t keep = 0;
    bool found = false;
    DWORD n;
    while (!found && ReadFile(h, buf + keep, 65536, &n, NULL) && n) {
        size_t len = keep + n;
        for (size_t i = 0; i + 16 <= len; i += 4) {
            if (*(uint32_t *)(buf + i) != 0xFEEF04BD) continue;
            uint32_t ms = *(uint32_t *)(buf + i + 8), ls = *(uint32_t *)(buf + i + 12);
            v[0] = ms >> 16; v[1] = ms & 0xFFFF; v[2] = ls >> 16; v[3] = ls & 0xFFFF;
            found = true;
            break;
        }
        /* the structure is DWORD aligned in the file; carry 16 bytes over */
        keep = len >= 16 ? 16 : len;
        memmove(buf, buf + len - keep, keep);
    }
    CloseHandle(h);
    return found;
}

static int cmp_version(const unsigned a[4], const char *s)
{
    unsigned b[4] = { 0, 0, 0, 0 };
    sscanf(s, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]);
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* Does @path satisfy Signature row @r (versions, sizes)? */
static bool signature_match(Inst *in, MsiTable *sg, int r, const char *path)
{
    char b[16], minv[64], maxv[64];
    snprintf(minv, sizeof(minv), "%s", msidb_str(&in->db, sg, r, msidb_col(sg, "MinVersion"), b));
    snprintf(maxv, sizeof(maxv), "%s", msidb_str(&in->db, sg, r, msidb_col(sg, "MaxVersion"), b));
    WCHAR wp[MAX_PATH];
    to_w(path, wp, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(wp, GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (minv[0] || maxv[0]) {
        unsigned v[4];
        if (!file_version(wp, v)) return false;
        if (minv[0] && cmp_version(v, minv) < 0) return false;
        if (maxv[0] && cmp_version(v, maxv) > 0) return false;
    }
    bool null;
    int mins = msidb_int(&in->db, sg, r, msidb_col(sg, "MinSize"), &null);
    if (!null && (int64_t)fa.nFileSizeLow < mins) return false;
    int maxs = msidb_int(&in->db, sg, r, msidb_col(sg, "MaxSize"), &null);
    if (!null && (int64_t)fa.nFileSizeLow > maxs) return false;
    return true;
}

/* The file of Signature row @r under @dir, down @depth levels of folders */
static bool find_signature_file(Inst *in, MsiTable *sg, int r, const char *dir, int depth, char *out, int cap)
{
    char b[16], name[256];
    snprintf(name, sizeof(name), "%s", msidb_str(&in->db, sg, r, msidb_col(sg, "FileName"), b));
    char *bar = strchr(name, '|');
    const char *lng = bar ? bar + 1 : name;
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s", dir);
    ensure_slash(path, sizeof(path));
    size_t base = strlen(path);
    snprintf(path + base, sizeof(path) - base, "%s", lng);
    if (signature_match(in, sg, r, path)) { snprintf(out, (size_t)cap, "%s", path); return true; }
    if (bar) {
        *bar = 0;
        snprintf(path + base, sizeof(path) - base, "%s", name);
        if (signature_match(in, sg, r, path)) { snprintf(out, (size_t)cap, "%s", path); return true; }
    }
    if (depth <= 0) return false;
    snprintf(path + base, sizeof(path) - base, "*");
    WCHAR wp[MAX_PATH * 2];
    to_w(path, wp, MAX_PATH * 2);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char sub[MAX_PATH], subdir[MAX_PATH * 2];
        to_u8(fd.cFileName, sub, sizeof(sub));
        path[base] = 0;
        snprintf(subdir, sizeof(subdir), "%s%s", path, sub);
        found = find_signature_file(in, sg, r, subdir, depth - 1, out, cap);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static bool dir_exists(const char *p)
{
    WCHAR w[MAX_PATH * 2];
    to_w(p, w, MAX_PATH * 2);
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* What a signature finds: a file (when the Signature table describes one)
 * or a folder, through RegLocator or DrLocator (Parent chains followed) */
static bool locate(Inst *in, const char *sig, char *out, int cap, int guard)
{
    if (guard > 16) return false;
    char b[16];
    MsiTable *sg = msidb_table(&in->db, "Signature");
    int sr = sg ? msidb_find(&in->db, sg, 0, sig, 0) : -1;
    MsiTable *rl = msidb_table(&in->db, "RegLocator");
    int l = rl ? msidb_find(&in->db, rl, 0, sig, 0) : -1;
    if (l >= 0) {
        int root = msidb_int(&in->db, rl, l, 1, NULL), type = msidb_int(&in->db, rl, l, 4, NULL);
        char key[512], name[256];
        format_str(in, msidb_str(&in->db, rl, l, 2, b), key, sizeof(key));
        format_str(in, msidb_str(&in->db, rl, l, 3, b), name, sizeof(name));
        WCHAR wkey[512], wname[256], wv[1024];
        to_w(key, wkey, 512);
        to_w(name, wname, 256);
        HKEY h;
        if (RegOpenKeyExW(root_key(in, root), wkey, 0, KEY_READ, &h)) return false;
        DWORD vt, size = sizeof(wv) - 2;
        LSTATUS e = RegQueryValueExW(h, name[0] ? wname : NULL, NULL, &vt, (BYTE *)wv, &size);
        RegCloseKey(h);
        if (e) return false;
        wv[size / 2] = 0;
        char v[1024];
        if (vt == REG_DWORD) snprintf(v, sizeof(v), "#%lu", (unsigned long)*(DWORD *)wv);
        else if (vt == REG_EXPAND_SZ) { WCHAR x[1024]; ExpandEnvironmentStringsW(wv, x, 1024); to_u8(x, v, sizeof(v)); }
        else to_u8(wv, v, sizeof(v));
        switch (type & 0x0F) {
        case 2:                                                  /* the raw value */
            snprintf(out, (size_t)cap, "%s", v);
            return true;
        case 1:                                                  /* a file path */
            if (v[0] == '"') { memmove(v, v + 1, strlen(v)); char *q = strchr(v, '"'); if (q) *q = 0; }
            if (sr >= 0) {
                char *slash = strrchr(v, '\\');
                if (!slash) return false;
                *slash = 0;
                return find_signature_file(in, sg, sr, v, 0, out, cap);
            }
            if (!dir_exists(v)) { WCHAR w[1024]; to_w(v, w, 1024); if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) return false; }
            snprintf(out, (size_t)cap, "%s", v);
            return true;
        default:                                                 /* a folder */
            if (sr >= 0) return find_signature_file(in, sg, sr, v, 0, out, cap);
            if (!dir_exists(v)) return false;
            snprintf(out, (size_t)cap, "%s", v);
            ensure_slash(out, (size_t)cap);
            return true;
        }
    }
    MsiTable *dl = msidb_table(&in->db, "DrLocator");
    for (int r = 0; dl && (r = msidb_find(&in->db, dl, 0, sig, r)) >= 0; r++) {
        char parent[80], path[MAX_PATH * 2], dir[MAX_PATH * 2];
        snprintf(parent, sizeof(parent), "%s", msidb_str(&in->db, dl, r, 1, b));
        format_str(in, msidb_str(&in->db, dl, r, 2, b), path, sizeof(path));
        bool null;
        int depth = msidb_int(&in->db, dl, r, 3, &null);
        if (null) depth = 0;
        if (parent[0]) {
            if (!locate(in, parent, dir, sizeof(dir), guard + 1)) continue;
            if (!dir_exists(dir)) {                              /* the parent found a file: its folder */
                char *slash = strrchr(dir, '\\');
                if (slash) slash[1] = 0;
            }
            ensure_slash(dir, sizeof(dir));
            strncat(dir, path, sizeof(dir) - strlen(dir) - 1);
        } else if (path[0] && path[1] != ':' && path[0] != '\\') {
            snprintf(dir, sizeof(dir), "C:\\%s", path);         /* relative: on the (one) fixed drive */
        } else {
            snprintf(dir, sizeof(dir), "%s", path[0] ? path : "C:\\");
        }
        if (sr >= 0) {
            if (find_signature_file(in, sg, sr, dir, depth, out, cap)) return true;
        } else if (dir_exists(dir)) {
            snprintf(out, (size_t)cap, "%s", dir);
            ensure_slash(out, (size_t)cap);
            return true;
        }
    }
    return false;
}

static void action_app_search(Inst *in)
{
    /* AppSearch(Property, Signature_): RegLocator and DrLocator, with the
     * Signature table for files */
    MsiTable *as = msidb_table(&in->db, "AppSearch");
    char b[16];
    for (int r = 0; as && r < as->nrows; r++) {
        char prop[80], sig[80], v[MAX_PATH * 2];
        snprintf(prop, sizeof(prop), "%s", msidb_str(&in->db, as, r, 0, b));
        snprintf(sig, sizeof(sig), "%s", msidb_str(&in->db, as, r, 1, b));
        if (!locate(in, sig, v, sizeof(v), 0)) continue;
        set_prop(in, prop, v);
        logf(in, "AppSearch: %s = %s", prop, v);
    }
}

static int run_product(const WCHAR *code, bool remove, int ui_level, MsiUi *ui, const WCHAR *props, char *err, int cap);

static void action_find_related(Inst *in)
{
    /* Upgrade(UpgradeCode, VersionMin, VersionMax, Language, Attributes, Remove, ActionProperty):
     * registered products with the same upgrade code go into ActionProperty */
    MsiTable *t = msidb_table(&in->db, "Upgrade");
    if (!t) return;
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, NOVA_INSTALLER_KEY, 0, KEY_READ, &h)) return;
    char b[16];
    for (DWORD i = 0; ; i++) {
        WCHAR sub[64];
        DWORD n = 64;
        if (RegEnumKeyExW(h, i, sub, &n, NULL, NULL, NULL, NULL)) break;
        HKEY p;
        if (RegOpenKeyExW(h, sub, 0, KEY_READ, &p)) continue;
        WCHAR up[64];
        DWORD type, size = sizeof(up);
        if (!RegQueryValueExW(p, L"UpgradeCode", NULL, &type, (BYTE *)up, &size)) {
            char upc[64], code[64];
            to_u8(up, upc, sizeof(upc));
            to_u8(sub, code, sizeof(code));
            for (int r = 0; r < t->nrows; r++) {
                if (_stricmp(msidb_str(&in->db, t, r, 0, b), upc)) continue;
                if (!strcmp(code, get_prop(in, "ProductCode"))) continue;
                const char *ap = msidb_str(&in->db, t, r, msidb_col(t, "ActionProperty"), b);
                if (!*ap) continue;
                char list[1024];
                snprintf(list, sizeof(list), "%s%s%s", get_prop(in, ap), *get_prop(in, ap) ? ";" : "", code);
                set_prop(in, ap, list);
                logf(in, "Related product %s found for %s", code, ap);
            }
        }
        RegCloseKey(p);
    }
    RegCloseKey(h);
}

static void action_remove_existing(Inst *in)
{
    MsiTable *t = msidb_table(&in->db, "Upgrade");
    char b[16];
    for (int r = 0; t && r < t->nrows; r++) {
        const char *ap = msidb_str(&in->db, t, r, msidb_col(t, "ActionProperty"), b);
        if (!*ap || !prop_set(in, ap)) continue;
        char list[1024];
        snprintf(list, sizeof(list), "%s", get_prop(in, ap));
        for (char *p = list; *p; ) {
            char *e = strchr(p, ';');
            if (e) *e = 0;
            WCHAR wc[64];
            to_w(p, wc, 64);
            logf(in, "Removing the earlier version %s", p);
            char err[256];
            run_product(wc, true, MSIUI_NONE, in->ui, NULL, err, sizeof(err));
            if (!e) break;
            p = e + 1;
        }
    }
}

/* -----------------------------------------------------------------------
 * Shortcut table: .lnk files through shell32's IShellLinkW.  A Target in
 * brackets is a formatted path; anything else names a feature (an
 * "advertised" shortcut), which points at the component's key file.
 * Icons come from the Icon table, saved under C:\Windows\Installer\{ProductCode}.
 * ----------------------------------------------------------------------- */
typedef struct LinkW LinkW;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(LinkW *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(LinkW *);
    ULONG   (WINAPI *Release)(LinkW *);
    HRESULT (WINAPI *GetPath)(LinkW *, LPWSTR, int, void *, DWORD);
    HRESULT (WINAPI *GetIDList)(LinkW *, void **);
    HRESULT (WINAPI *SetIDList)(LinkW *, const void *);
    HRESULT (WINAPI *GetDescription)(LinkW *, LPWSTR, int);
    HRESULT (WINAPI *SetDescription)(LinkW *, LPCWSTR);
    HRESULT (WINAPI *GetWorkingDirectory)(LinkW *, LPWSTR, int);
    HRESULT (WINAPI *SetWorkingDirectory)(LinkW *, LPCWSTR);
    HRESULT (WINAPI *GetArguments)(LinkW *, LPWSTR, int);
    HRESULT (WINAPI *SetArguments)(LinkW *, LPCWSTR);
    HRESULT (WINAPI *GetHotkey)(LinkW *, WORD *);
    HRESULT (WINAPI *SetHotkey)(LinkW *, WORD);
    HRESULT (WINAPI *GetShowCmd)(LinkW *, int *);
    HRESULT (WINAPI *SetShowCmd)(LinkW *, int);
    HRESULT (WINAPI *GetIconLocation)(LinkW *, LPWSTR, int, int *);
    HRESULT (WINAPI *SetIconLocation)(LinkW *, LPCWSTR, int);
    HRESULT (WINAPI *SetRelativePath)(LinkW *, LPCWSTR, DWORD);
    HRESULT (WINAPI *Resolve)(LinkW *, HWND, DWORD);
    HRESULT (WINAPI *SetPath)(LinkW *, LPCWSTR);
} LinkWVtbl;
struct LinkW { const LinkWVtbl *v; };

typedef struct PFile PFile;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(PFile *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(PFile *);
    ULONG   (WINAPI *Release)(PFile *);
    HRESULT (WINAPI *GetClassID)(PFile *, CLSID *);
    HRESULT (WINAPI *IsDirty)(PFile *);
    HRESULT (WINAPI *Load)(PFile *, LPCWSTR, DWORD);
    HRESULT (WINAPI *Save)(PFile *, LPCWSTR, BOOL);
} PFileVtbl;
struct PFile { const PFileVtbl *v; };

static const GUID CLSID_ShellLink_ = { 0x00021401, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IShellLinkW_ = { 0x000214F9, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IPersistFile_ = { 0x0000010B, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

/* The key file of a component (its KeyPath names a File row) */
static void component_key_file(Inst *in, const char *comp, char *out, int cap)
{
    out[0] = 0;
    int c = msidb_find(&in->db, in->component, 0, comp, 0);
    if (c < 0) return;
    char b[16];
    const char *key = msidb_str(&in->db, in->component, c, msidb_col(in->component, "KeyPath"), b);
    if (*key && msidb_find(&in->db, in->file, 0, key, 0) >= 0) file_path(in, key, out, cap);
    else dir_path(in, msidb_str(&in->db, in->component, c, 2, b), out, cap);
}

static void shortcut_path(Inst *in, MsiTable *t, int r, char *out, int cap)
{
    char b[16], dir[MAX_PATH], name[MAX_PATH];
    dir_path(in, msidb_str(&in->db, t, r, 1, b), dir, sizeof(dir));
    long_name(msidb_str(&in->db, t, r, 2, b), name, sizeof(name));
    snprintf(out, (size_t)cap, "%s%s.lnk", dir, name);
}

/* An Icon table entry as a file, for shortcuts to point at */
static bool save_icon(Inst *in, const char *icon, char *out, int cap)
{
    char sname[128];
    snprintf(sname, sizeof(sname), "Icon.%s", icon);
    size_t size = 0;
    void *data = msidb_read_stream(&in->db, sname, &size);
    if (!data) return false;
    snprintf(out, (size_t)cap, "C:\\Windows\\Installer\\%s\\", get_prop(in, "ProductCode"));
    make_dirs(in, out);
    size_t n = strlen(out);
    snprintf(out + n, (size_t)cap - n, "%s", icon);
    WCHAR w[MAX_PATH];
    to_w(out, w, MAX_PATH);
    rb_file(in, w);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = false;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        ok = WriteFile(h, data, (DWORD)size, &wr, NULL) && wr == size;
        CloseHandle(h);
    }
    free(data);
    return ok;
}

static void action_create_shortcuts(Inst *in)
{
    MsiTable *t = msidb_table(&in->db, "Shortcut");
    if (!t) return;
    CoInitialize(NULL);
    char b[16];
    int cargs = msidb_col(t, "Arguments"), cdesc = msidb_col(t, "Description"), chot = msidb_col(t, "Hotkey"),
        cicon = msidb_col(t, "Icon_"), cidx = msidb_col(t, "IconIndex"), cshow = msidb_col(t, "ShowCmd"),
        cwk = msidb_col(t, "WkDir");
    for (int r = 0; r < t->nrows; r++) {
        const char *comp = msidb_str(&in->db, t, r, 3, b);
        if (!comp_enabled(in, comp)) continue;
        char lnk[MAX_PATH], target[MAX_PATH * 2], tmp[MAX_PATH * 2];
        shortcut_path(in, t, r, lnk, sizeof(lnk));
        const char *raw = msidb_str(&in->db, t, r, 4, b);
        if (raw[0] == '[') format_str(in, raw, target, sizeof(target));
        else component_key_file(in, comp, target, sizeof(target));     /* advertised: the feature's component */
        LinkW *link = NULL;
        if (FAILED(CoCreateInstance(&CLSID_ShellLink_, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW_, (void **)&link)) || !link) {
            logf(in, "Shortcut %s: shell32 has no ShellLink", lnk);
            continue;
        }
        WCHAR w[MAX_PATH * 2];
        to_w(target, w, MAX_PATH * 2);
        link->v->SetPath(link, w);
        format_str(in, msidb_str(&in->db, t, r, cargs, b), tmp, sizeof(tmp));
        if (tmp[0]) { to_w(tmp, w, MAX_PATH * 2); link->v->SetArguments(link, w); }
        format_str(in, msidb_str(&in->db, t, r, cdesc, b), tmp, sizeof(tmp));
        if (tmp[0]) { to_w(tmp, w, MAX_PATH * 2); link->v->SetDescription(link, w); }
        const char *wk = msidb_str(&in->db, t, r, cwk, b);
        if (*wk) { dir_path(in, wk, tmp, sizeof(tmp)); to_w(tmp, w, MAX_PATH * 2); link->v->SetWorkingDirectory(link, w); }
        else {
            /* like Windows: the target's folder */
            snprintf(tmp, sizeof(tmp), "%s", target);
            char *bs = strrchr(tmp, '\\');
            if (bs) { *bs = 0; to_w(tmp, w, MAX_PATH * 2); link->v->SetWorkingDirectory(link, w); }
        }
        bool null;
        int hot = msidb_int(&in->db, t, r, chot, &null);
        if (!null && hot) link->v->SetHotkey(link, (WORD)hot);
        int show = msidb_int(&in->db, t, r, cshow, &null);
        if (!null && show) link->v->SetShowCmd(link, show);
        const char *icon = msidb_str(&in->db, t, r, cicon, b);
        char iconpath[MAX_PATH];
        if (*icon && save_icon(in, icon, iconpath, sizeof(iconpath))) {
            to_w(iconpath, w, MAX_PATH * 2);
            link->v->SetIconLocation(link, w, msidb_int(&in->db, t, r, cidx, NULL));
        }
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s", lnk);
        char *bs = strrchr(dir, '\\');
        if (bs) { bs[1] = 0; make_dirs(in, dir); }
        PFile *pf = NULL;
        HRESULT hr = E_FAIL;
        if (SUCCEEDED(link->v->QueryInterface(link, &IID_IPersistFile_, (void **)&pf)) && pf) {
            to_w(lnk, w, MAX_PATH * 2);
            rb_file(in, w);
            hr = pf->v->Save(pf, w, TRUE);
            pf->v->Release(pf);
        }
        link->v->Release(link);
        if (SUCCEEDED(hr)) logf(in, "Shortcut %s -> %s", lnk, target);
        else logf(in, "Could not save shortcut %s (%#lx)", lnk, (unsigned long)hr);
    }
}

static void action_remove_shortcuts(Inst *in)
{
    MsiTable *t = msidb_table(&in->db, "Shortcut");
    char b[16];
    for (int r = 0; t && r < t->nrows; r++) {
        if (!comp_enabled(in, msidb_str(&in->db, t, r, 3, b))) continue;
        char lnk[MAX_PATH];
        shortcut_path(in, t, r, lnk, sizeof(lnk));
        WCHAR w[MAX_PATH];
        to_w(lnk, w, MAX_PATH);
        if (rb_delete_file(in, w)) logf(in, "Removed shortcut %s", lnk);
        char *bs = strrchr(lnk, '\\');
        if (bs) { *bs = 0; remove_folder_chain(in, lnk); }
    }
    /* the saved icons */
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "C:\\Windows\\Installer\\%s", get_prop(in, "ProductCode"));
    WCHAR wd[MAX_PATH], pat[MAX_PATH];
    to_w(dir, wd, MAX_PATH);
    _snwprintf(pat, MAX_PATH, L"%s\\*", wd);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            WCHAR p[MAX_PATH];
            _snwprintf(p, MAX_PATH, L"%s\\%s", wd, fd.cFileName);
            size_t pl = wcslen(p);
            if (pl > 4 && (!_wcsicmp(p + pl - 4, L".mst") || !_wcsicmp(p + pl - 4, L".msp"))) continue;  /* (kept with the product) */
            rb_delete_file(in, p);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    rb_remove_dir(in, wd);
}

/* -----------------------------------------------------------------------
 * Services: ServiceInstall registers them with the service control
 * manager (advapi32), ServiceControl starts, stops and deletes them.
 * ----------------------------------------------------------------------- */

static void action_install_services(Inst *in)
{
    MsiTable *t = msidb_table(&in->db, "ServiceInstall");
    if (!t) return;
    SC_H scm = OpenSCManagerW(NULL, NULL, SCM_ALL_ACCESS);
    if (!scm) { logf(in, "No service control manager"); return; }
    char b[16];
    int ccomp = msidb_col(t, "Component_"), cdesc = msidb_col(t, "Description"), cargs = msidb_col(t, "Arguments");
    for (int r = 0; r < t->nrows; r++) {
        const char *comp = msidb_str(&in->db, t, r, ccomp, b);
        if (!comp_enabled(in, comp)) continue;
        char name[256], disp[256], group[256], deps[512], user[256], pass[256], args[512], desc[1024], exe[MAX_PATH];
        format_str(in, msidb_str(&in->db, t, r, 1, b), name, sizeof(name));
        format_str(in, msidb_str(&in->db, t, r, 2, b), disp, sizeof(disp));
        int type = msidb_int(&in->db, t, r, 3, NULL), start = msidb_int(&in->db, t, r, 4, NULL),
            errc = msidb_int(&in->db, t, r, 5, NULL);
        format_str(in, msidb_str(&in->db, t, r, 6, b), group, sizeof(group));
        format_str(in, msidb_str(&in->db, t, r, 7, b), deps, sizeof(deps));
        format_str(in, msidb_str(&in->db, t, r, 8, b), user, sizeof(user));
        format_str(in, msidb_str(&in->db, t, r, 9, b), pass, sizeof(pass));
        format_str(in, msidb_str(&in->db, t, r, cargs, b), args, sizeof(args));
        format_str(in, msidb_str(&in->db, t, r, cdesc, b), desc, sizeof(desc));
        component_key_file(in, comp, exe, sizeof(exe));
        char cmd[MAX_PATH + 600];
        snprintf(cmd, sizeof(cmd), "\"%s\"%s%s", exe, args[0] ? " " : "", args);
        /* Dependencies: names separated by [~] (already dropped by formatting: re-split the raw text) */
        WCHAR wdeps[512];
        int nd = 0;
        const char *rawdeps = msidb_str(&in->db, t, r, 7, b);
        for (const char *p = rawdeps; *p && nd < 500; ) {
            const char *e = strstr(p, "[~]");
            size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l) {
                char one[256];
                snprintf(one, sizeof(one), "%.*s", (int)l, p);
                WCHAR w1[256];
                to_w(one, w1, 256);
                for (int i = 0; w1[i] && nd < 500; i++) wdeps[nd++] = w1[i];
                wdeps[nd++] = 0;
            }
            if (!e) break;
            p = e + 3;
        }
        wdeps[nd++] = 0;
        wdeps[nd] = 0;
        WCHAR wname[256], wdisp[256], wcmd[MAX_PATH + 600], wgroup[256], wuser[256], wpass[256];
        to_w(name, wname, 256); to_w(disp, wdisp, 256); to_w(cmd, wcmd, MAX_PATH + 600);
        to_w(group, wgroup, 256); to_w(user, wuser, 256); to_w(pass, wpass, 256);
        SC_H svc = CreateServiceW(scm, wname, disp[0] ? wdisp : wname, SVC_ALL_ACCESS, (DWORD)type, (DWORD)start, (DWORD)errc,
                                  wcmd, group[0] ? wgroup : NULL, NULL, nd > 2 ? wdeps : NULL,
                                  user[0] ? wuser : NULL, pass[0] ? wpass : NULL);
        if (svc) rb_note(in, RB_SERVICE_NEW, name, NULL);
        else if (GetLastError() == 1073 /* ERROR_SERVICE_EXISTS */) {
            WCHAR skey[300];
            _snwprintf(skey, 300, L"%s%s", SERVICES_KEY, wname);
            rb_key(in, HKEY_LOCAL_MACHINE, skey);
            svc = OpenServiceW(scm, wname, SVC_ALL_ACCESS);
            if (svc) ChangeServiceConfigW(svc, (DWORD)type, (DWORD)start, (DWORD)errc, wcmd, group[0] ? wgroup : NULL, NULL,
                                          nd > 2 ? wdeps : NULL, user[0] ? wuser : NULL, pass[0] ? wpass : NULL,
                                          disp[0] ? wdisp : NULL);
        }
        if (!svc) { logf(in, "Could not install service %s (error %lu)", name, GetLastError()); continue; }
        if (desc[0]) {
            WCHAR wdesc[1024];
            to_w(desc, wdesc, 1024);
            WCHAR *pd = wdesc;
            ChangeServiceConfig2W(svc, 1 /* SERVICE_CONFIG_DESCRIPTION */, &pd);
        }
        CloseServiceHandle(svc);
        logf(in, "Installed service %s: %s", name, cmd);
    }
    CloseServiceHandle(scm);
}

/* ServiceControl events: 0x1/0x10 start, 0x2/0x20 stop, 0x8/0x80 delete
 * (low bits on install, high bits on uninstall) */
static void service_control(Inst *in, int what)
{
    MsiTable *t = msidb_table(&in->db, "ServiceControl");
    if (!t) return;
    SC_H scm = OpenSCManagerW(NULL, NULL, SCM_ALL_ACCESS);
    if (!scm) return;
    char b[16];
    int ccomp = msidb_col(t, "Component_"), cwait = msidb_col(t, "Wait"), cargs = msidb_col(t, "Arguments");
    for (int r = 0; r < t->nrows; r++) {
        if (!comp_enabled(in, msidb_str(&in->db, t, r, ccomp, b))) continue;
        int ev = msidb_int(&in->db, t, r, 2, NULL);
        int bit = in->remove ? what << 4 : what;
        if (!(ev & bit)) continue;
        char name[256];
        format_str(in, msidb_str(&in->db, t, r, 1, b), name, sizeof(name));
        WCHAR wname[256];
        to_w(name, wname, 256);
        SC_H svc = OpenServiceW(scm, wname, SVC_ALL_ACCESS);
        if (!svc) continue;
        bool wait = msidb_int(&in->db, t, r, cwait, NULL) != 0;
        if (what == 1) {
            char args[512];
            format_str(in, msidb_str(&in->db, t, r, cargs, b), args, sizeof(args));
            WCHAR wargs[512];
            to_w(args, wargs, 512);
            const WCHAR *argv[1] = { wargs };
            BOOL ok = StartServiceW(svc, args[0] ? 1 : 0, args[0] ? argv : NULL);
            logf(in, "Start service %s: %s", name, ok ? "started" : "failed");
            if (ok) rb_note(in, RB_SERVICE_STARTED, name, NULL);
            if (!ok && GetLastError() != 1056 /* already running */ && wait)
                logf(in, "Service %s did not start (error %lu)", name, GetLastError());
        } else if (what == 2) {
            DWORD st[7];
            bool running = QueryServiceStatus(svc, st) && st[1] != 1 /* SERVICE_STOPPED */;
            if (ControlService(svc, 1 /* SERVICE_CONTROL_STOP */, st)) {
                if (running) rb_note(in, RB_SERVICE_STOPPED, name, NULL);
                for (int i = 0; wait && i < 300; i++) {
                    if (!QueryServiceStatus(svc, st) || st[1] == 1 /* SERVICE_STOPPED */) break;
                    eng_pump(in);
                    Sleep(100);
                }
                logf(in, "Stopped service %s", name);
            }
        } else if (what == 8) {
            WCHAR skey[300];
            _snwprintf(skey, 300, L"%s%s", SERVICES_KEY, wname);
            rb_key(in, HKEY_LOCAL_MACHINE, skey);
            if (DeleteService(svc)) logf(in, "Deleted service %s", name);
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

/* -----------------------------------------------------------------------
 * Custom actions
 *
 * Type = source and kind in the low six bits (DLL 1/17, EXE 2/18/34/50,
 * text: error 19, directory 35, property 51; scripts 5/6/21/22/37/38/53/54;
 * nested installs 7/23/39) plus 0x40 ignore the result, 0x80 don't wait,
 * 0x100/0x200 run once, 0x400 deferred (0x500 rollback, 0x600 commit).
 * ----------------------------------------------------------------------- */
#define CA_CONTINUE   0x40
#define CA_ASYNC      0x80
#define CA_FIRSTSEQ   0x100
#define CA_ONCE       0x200
#define CA_INSCRIPT   0x400
#define CA_ROLLBACK   (CA_INSCRIPT | CA_FIRSTSEQ)
#define CA_COMMIT     (CA_INSCRIPT | CA_ONCE)

static bool action_ran_in_ui(Inst *in, const char *name)
{
    for (int i = 0; i < in->nran; i++) if (!strcmp(in->ran[i], name)) return true;
    return false;
}

static void mark_ran(Inst *in, const char *name)
{
    if (!in->in_ui || action_ran_in_ui(in, name)) return;
    in->ran = realloc(in->ran, (size_t)(in->nran + 1) * sizeof(char *));
    in->ran[in->nran++] = strdup(name);
}

/* A Binary table stream written to a temporary file (for DLL and EXE actions) */
static bool binary_to_file(Inst *in, const char *key, const char *ext, WCHAR *out)
{
    char sname[128];
    snprintf(sname, sizeof(sname), "Binary.%s", key);
    size_t size = 0;
    void *data = msidb_read_stream(&in->db, sname, &size);
    if (!data) { logf(in, "Binary %s is missing", key); return false; }
    make_dirs(in, "C:\\Windows\\Installer\\");
    static unsigned counter;
    WCHAR wext[16];
    to_w(ext, wext, 16);
    _snwprintf(out, MAX_PATH, L"C:\\Windows\\Installer\\MSI%04X%04X.%s", (unsigned)(GetTickCount() & 0xFFFF), ++counter, wext);
    HANDLE h = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = false;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        ok = WriteFile(h, data, (DWORD)size, &wr, NULL) && wr == size;
        CloseHandle(h);
    }
    free(data);
    return ok;
}

/* Run a program: wait for it unless asynchronous; its exit code is the result */
static UINT run_program(Inst *in, const char *cmd, const char *workdir, int type)
{
    WCHAR wcmd[4096], wdir[MAX_PATH];
    to_w(cmd, wcmd, 4096);
    if (workdir) to_w(workdir, wdir, MAX_PATH);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    logf(in, "Running %s%s%s", cmd, workdir ? " in " : "", workdir ? workdir : "");
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, 0, NULL, workdir && *workdir ? wdir : NULL, &si, &pi)) {
        logf(in, "Could not start it (error %lu)", GetLastError());
        return ERROR_INSTALL_FAILURE;
    }
    CloseHandle(pi.hThread);
    if (type & CA_ASYNC) { CloseHandle(pi.hProcess); return 0; }
    DWORD code = api_wait_process(in, pi.hProcess);
    CloseHandle(pi.hProcess);
    logf(in, "It exited with %lu", (unsigned long)code);
    return code ? ERROR_INSTALL_FAILURE : 0;
}

static UINT run_dll(Inst *in, const WCHAR *dll, const char *entry)
{
    bool crashed;
    int r = api_run_dll_action(in, dll, entry, &crashed);
    return (UINT)r;
}

static bool run_custom_action(Inst *in, const char *name)
{
    MsiTable *t = msidb_table(&in->db, "CustomAction");
    int r = msidb_find(&in->db, t, 0, name, 0);
    if (r < 0) {
        logf(in, "Action %s: unknown, skipped", name);
        return true;
    }
    char b[16], source[256], target[4096];
    int type = msidb_int(&in->db, t, r, 1, NULL);
    snprintf(source, sizeof(source), "%s", msidb_str(&in->db, t, r, 2, b));
    const char *raw_target = msidb_str(&in->db, t, r, 3, b);
    int kind = type & 0x3F;
    /* scheduling: once when the UI sequence already ran it */
    if (!in->in_ui && (type & (CA_FIRSTSEQ | CA_ONCE)) && !(type & CA_INSCRIPT) && action_ran_in_ui(in, name)) {
        logf(in, "Custom action %s already ran in the UI sequence", name);
        return true;
    }
    mark_ran(in, name);
    if ((type & 0x700) == CA_ROLLBACK && !in->rolling_back) {
        /* a rollback action: it runs only if the installation fails, with
         * the CustomActionData it has now */
        if (in->rb_on) { rb_note(in, RB_ACTION, name, get_prop(in, name)); logf(in, "Rollback action %s scheduled", name); }
        else logf(in, "Rollback action %s skipped: rollback is off", name);
        return true;
    }
    if ((type & CA_COMMIT) == CA_COMMIT && in->sequence_depth >= 0 && !in->modes[RUNMODE_COMMIT]) {
        in->commit = realloc(in->commit, (size_t)(in->ncommit + 1) * sizeof(char *));
        in->commit[in->ncommit++] = strdup(name);
        logf(in, "Commit action %s runs at the end", name);
        return true;
    }
    bool deferred = (type & CA_INSCRIPT) != 0;
    char *saved_cad = NULL;
    if (deferred) {
        /* a deferred action sees its data as CustomActionData */
        saved_cad = strdup(get_prop(in, "CustomActionData"));
        set_prop(in, "CustomActionData", get_prop(in, name));
        in->modes[RUNMODE_SCHEDULED] = true;
    }
    UINT result = 0;
    switch (kind) {
    case 51:                                   /* set property */
        format_str(in, raw_target, target, sizeof(target));
        set_prop(in, source, target);
        logf(in, "Property %s = %s", source, target);
        break;
    case 35:                                   /* set directory */
        format_str(in, raw_target, target, sizeof(target));
        eng_set_target_path(in, source, target);
        logf(in, "Directory %s = %s", source, target);
        break;
    case 19:                                   /* error message */
        format_str(in, raw_target, target, sizeof(target));
        fail(in, MSI_ERROR_FAILURE, "%s", target[0] ? target : name);
        result = ERROR_INSTALL_FAILURE;
        type &= ~CA_CONTINUE;
        break;
    case 1: {                                  /* DLL in the Binary table */
        WCHAR dll[MAX_PATH];
        if (!binary_to_file(in, source, "tmp", dll)) { result = ERROR_INSTALL_FAILURE; break; }
        format_str(in, raw_target, target, sizeof(target));
        logf(in, "Custom action %s: %s in Binary %s", name, target, source);
        result = run_dll(in, dll, target);
        DeleteFileW(dll);
        break;
    }
    case 17: {                                 /* DLL installed with the product */
        char path[MAX_PATH * 2];
        file_path(in, source, path, sizeof(path));
        WCHAR dll[MAX_PATH];
        to_w(path, dll, MAX_PATH);
        format_str(in, raw_target, target, sizeof(target));
        logf(in, "Custom action %s: %s in %s", name, target, path);
        result = run_dll(in, dll, target);
        break;
    }
    case 2: {                                  /* program in the Binary table */
        WCHAR exe[MAX_PATH];
        if (!binary_to_file(in, source, "exe", exe)) { result = ERROR_INSTALL_FAILURE; break; }
        char u8[MAX_PATH], cmd[4096 + MAX_PATH];
        to_u8(exe, u8, sizeof(u8));
        format_str(in, raw_target, target, sizeof(target));
        snprintf(cmd, sizeof(cmd), "\"%s\"%s%s", u8, target[0] ? " " : "", target);
        result = run_program(in, cmd, NULL, type);
        if (!(type & CA_ASYNC)) DeleteFileW(exe);
        break;
    }
    case 18: {                                 /* installed program */
        char path[MAX_PATH * 2], cmd[4096 + MAX_PATH * 2];
        file_path(in, source, path, sizeof(path));
        format_str(in, raw_target, target, sizeof(target));
        snprintf(cmd, sizeof(cmd), "\"%s\"%s%s", path, target[0] ? " " : "", target);
        result = run_program(in, cmd, NULL, type);
        break;
    }
    case 34: {                                 /* command line in a working folder */
        char dir[MAX_PATH * 2];
        dir_path(in, source, dir, sizeof(dir));
        format_str(in, raw_target, target, sizeof(target));
        result = run_program(in, target, dir, type);
        break;
    }
    case 50: {                                 /* program named by a property */
        char exe[MAX_PATH * 2], cmd[4096 + MAX_PATH * 2];
        format_str(in, get_prop(in, source), exe, sizeof(exe));
        format_str(in, raw_target, target, sizeof(target));
        if (!exe[0]) { logf(in, "Custom action %s: property %s is empty", name, source); result = ERROR_INSTALL_FAILURE; break; }
        snprintf(cmd, sizeof(cmd), exe[0] == '"' ? "%s%s%s" : "\"%s\"%s%s", exe, target[0] ? " " : "", target);
        result = run_program(in, cmd, NULL, type);
        break;
    }
    case 5: case 6: case 21: case 22: case 37: case 38: case 53: case 54:
        logf(in, "Custom action %s is a %s script: NovaOS has no script engine, skipped", name,
             (kind & 7) == 5 ? "JScript" : "VBScript");
        break;
    default:
        logf(in, "Custom action %s (type %d) skipped: nested installations are not supported", name, type);
        break;
    }
    if (deferred) {
        set_prop(in, "CustomActionData", saved_cad);
        free(saved_cad);
        in->modes[RUNMODE_SCHEDULED] = false;
    }
    if (result == ERROR_NO_MORE_ITEMS) {        /* skip the rest of the sequence, successfully */
        logf(in, "Custom action %s: the rest of the sequence is skipped", name);
        in->stop_sequence = true;
        return true;
    }
    if (result == 1626 /* ERROR_FUNCTION_NOT_CALLED */) result = 0;
    if (result && (type & CA_CONTINUE)) {
        logf(in, "Custom action %s returned %u (ignored)", name, result);
        return true;
    }
    if (result) {
        if (result == MSI_ERROR_USEREXIT) fail(in, MSI_ERROR_USEREXIT, "Cancelled by the user");
        else fail(in, MSI_ERROR_FAILURE, "Custom action %s failed (%u)", name, result);
        return false;
    }
    return true;
}

static void run_commit_actions(Inst *in)
{
    in->modes[RUNMODE_COMMIT] = true;
    for (int i = 0; i < in->ncommit; i++) {
        logf(in, "Commit action: %s", in->commit[i]);
        run_custom_action(in, in->commit[i]);
        free(in->commit[i]);
    }
    free(in->commit);
    in->commit = NULL;
    in->ncommit = 0;
    in->modes[RUNMODE_COMMIT] = false;
}

/* -----------------------------------------------------------------------
 * The sequence
 * ----------------------------------------------------------------------- */
static bool run_sequence(Inst *in, const char *table);

static bool is_noop_action(const char *a)
{
    static const char *noop[] = {
        "CostInitialize", "FileCost", "InstallValidate", "InstallInitialize", "ProcessComponents", "ValidateProductID",
        "MigrateFeatureStates", "PublishFeatures", "PublishProduct", "RegisterUser", "UnpublishFeatures",
        "UnpublishComponents", "RegisterComponents", "UnregisterComponents", "SetODBCFolders", "InstallODBC",
        "RemoveODBC", "AllocateRegistrySpace", "ScheduleReboot", "ForceReboot", "IsolateComponents",
        "RegisterProgIdInfo", "UnregisterProgIdInfo", "DuplicateFiles", "RemoveDuplicateFiles", "PatchFiles",
        "BindImage", "SelfRegModules", "SelfUnregModules", "RemoveIniValues", "WriteIniValues", "RegisterFonts",
        "UnregisterFonts", "RegisterTypeLibraries", "UnregisterTypeLibraries", "RegisterExtensionInfo",
        "UnregisterExtensionInfo", "RegisterMIMEInfo", "UnregisterMIMEInfo", "RegisterClassInfo", "UnregisterClassInfo",
        "MoveFiles", "InstallAdminPackage", "InstallSFPCatalogFile", "InstallExecute", "InstallExecuteAgain",
        "RMCCPSearch", "CCPSearch", "MsiPublishAssemblies", "MsiUnpublishAssemblies", "PublishComponents",
        "MsiConfigureServices", "SetupProgress", "ResolveSource", "LaunchConditions_",
        "PrepareDlg_", "MsiUnpublishAssemblies_", "RemoveFile_",
    };
    for (size_t i = 0; i < sizeof(noop) / sizeof(noop[0]); i++) if (!strcmp(a, noop[i])) return true;
    return false;
}

/* The UI sequence hands its feature choices to the execute sequence the
 * way Windows' client does: as ADDLOCAL and REMOVE */
static void pass_feature_choices(Inst *in)
{
    if (!in->feature_on || !in->feature) return;
    char add[4096] = "", rem[4096] = "", b[16];
    for (int i = 0; i < in->feature->nrows; i++) {
        char *list = in->feature_on[i] ? add : rem;
        size_t n = strlen(list);
        snprintf(list + n, sizeof(add) - n, "%s%s", n ? "," : "", msidb_str(&in->db, in->feature, i, 0, b));
    }
    set_prop(in, "ADDLOCAL", add);
    if (in->installed) set_prop(in, "REMOVE", rem);
    logf(in, "Features chosen: ADDLOCAL=%s", add);
}

/* InstallExecuteSequence, journaled: a failure before InstallFinalize
 * (which commits) puts everything back */
static bool execute_sequence(Inst *in)
{
    rb_begin(in);
    bool ok = run_sequence(in, "InstallExecuteSequence");
    if (!ok) rb_run(in);
    else rb_commit(in);
    return ok;
}

static bool action_execute(Inst *in)
{
    in->executed = true;
    if (!strcmp(get_prop(in, "REMOVE"), "ALL")) in->remove = true;
    else if (in->dlg) pass_feature_choices(in);
    bool was_ui = in->in_ui;
    in->in_ui = false;
    logf(in, "Running InstallExecuteSequence");
    bool ok = execute_sequence(in);
    in->in_ui = was_ui;
    in->stop_sequence = false;
    return ok;
}

static bool run_action(Inst *in, const char *a)
{
    if (!strcmp(a, "CostFinalize"))      { select_features(in); resolve_directories(in); set_prop(in, "CostingComplete", "1"); return true; }
    if (!strcmp(a, "LaunchConditions"))  return action_launch_conditions(in);
    if (!strcmp(a, "AppSearch"))         { action_app_search(in); return true; }
    if (!strcmp(a, "FindRelatedProducts")) { action_find_related(in); return true; }
    if (!strcmp(a, "RemoveExistingProducts")) { action_remove_existing(in); return true; }
    if (!strcmp(a, "CreateFolders"))     { if (!in->remove) action_create_folders(in); return true; }
    if (!strcmp(a, "InstallFiles"))      return in->remove ? true : action_install_files(in);
    if (!strcmp(a, "WriteRegistryValues")) { if (!in->remove) action_write_registry(in); return true; }
    if (!strcmp(a, "RemoveRegistryValues")) { if (in->remove) { action_remove_registry(in); in->removed_registry = true; } return true; }
    if (!strcmp(a, "RemoveFiles"))       { if (in->remove) { action_remove_files(in); in->removed_files = true; } return true; }
    if (!strcmp(a, "RemoveFolders"))     { if (in->remove) { action_remove_folders(in); in->removed_folders = true; } return true; }
    if (!strcmp(a, "RegisterProduct"))   { if (!in->remove) action_register_product(in); return true; }
    if (!strcmp(a, "CreateShortcuts"))   { if (!in->remove) action_create_shortcuts(in); return true; }
    if (!strcmp(a, "RemoveShortcuts"))   { if (in->remove) { action_remove_shortcuts(in); in->removed_shortcuts = true; } return true; }
    if (!strcmp(a, "WriteEnvironmentStrings")) { if (!in->remove) action_write_env(in); return true; }
    if (!strcmp(a, "RemoveEnvironmentStrings")) { if (in->remove) { action_remove_env(in); in->removed_env = true; } return true; }
    if (!strcmp(a, "InstallServices"))   { if (!in->remove) action_install_services(in); return true; }
    if (!strcmp(a, "StartServices"))     { service_control(in, 1); return true; }
    if (!strcmp(a, "StopServices"))      { service_control(in, 2); return true; }
    if (!strcmp(a, "DeleteServices"))    { service_control(in, 8); return true; }
    if (!strcmp(a, "ExecuteAction"))     return action_execute(in);
    if (!strcmp(a, "InstallFinalize")) {
        if (in->remove) {
            /* a package whose sequence lacks the removal actions still gets cleaned up */
            if (!in->removed_shortcuts) action_remove_shortcuts(in);
            if (!in->removed_registry) action_remove_registry(in);
            if (!in->removed_env) action_remove_env(in);
            if (!in->removed_files) action_remove_files(in);
            if (!in->removed_folders) action_remove_folders(in);
            action_unregister_product(in);
        }
        rb_commit(in);                                       /* no rollback from here on */
        run_commit_actions(in);
        return true;
    }
    if (!strcmp(a, "DisableRollback")) { logf(in, "Rollback disabled by the package"); rb_commit(in); return true; }
    if (is_noop_action(a)) return true;
    if (in->dlg && dlg_exists(in->dlg, a)) {
        int r = dlg_run(in->dlg, a);
        if (r == MSI_ERROR_USEREXIT) { fail(in, MSI_ERROR_USEREXIT, "Cancelled by the user"); return false; }
        if (r) { fail(in, r, "The dialog %s ended the installation", a); return false; }
        return true;
    }
    return run_custom_action(in, a);
}

typedef struct { char name[80]; char *condition; int seq; } SeqItem;

static int cmp_seqitem(const void *a, const void *b) { return ((const SeqItem *)a)->seq - ((const SeqItem *)b)->seq; }

static bool run_sequence(Inst *in, const char *table)
{
    MsiTable *t = msidb_table(&in->db, table);
    bool execute = !strcmp(table, "InstallExecuteSequence");
    SeqItem *items;
    int n = 0;
    char b[16];
    if (t && t->nrows) {
        items = calloc((size_t)t->nrows, sizeof(SeqItem));
        for (int r = 0; r < t->nrows; r++) {
            int seq = msidb_int(&in->db, t, r, 2, NULL);
            if (seq <= 0) continue;              /* negative: only on error/exit */
            snprintf(items[n].name, sizeof(items[n].name), "%s", msidb_str(&in->db, t, r, 0, b));
            items[n].condition = strdup(msidb_str(&in->db, t, r, 1, b));
            items[n].seq = seq;
            n++;
        }
        qsort(items, (size_t)n, sizeof(SeqItem), cmp_seqitem);
    } else if (execute) {
        static const char *std[] = { "LaunchConditions", "FindRelatedProducts", "AppSearch", "CostInitialize",
            "FileCost", "CostFinalize", "InstallValidate", "RemoveExistingProducts", "InstallInitialize",
            "ProcessComponents", "StopServices", "DeleteServices", "RemoveRegistryValues", "RemoveShortcuts",
            "RemoveFiles", "RemoveFolders", "CreateFolders", "InstallFiles", "WriteRegistryValues",
            "InstallServices", "StartServices", "CreateShortcuts", "RegisterProduct", "InstallFinalize" };
        n = (int)(sizeof(std) / sizeof(std[0]));
        items = calloc((size_t)n, sizeof(SeqItem));
        for (int i = 0; i < n; i++) { snprintf(items[i].name, sizeof(items[i].name), "%s", std[i]); items[i].condition = strdup(""); }
    } else {
        return true;
    }
    bool ok = true;
    bool costed = false;
    in->sequence_depth++;
    for (int i = 0; ok && i < n && !in->stop_sequence; i++) {
        if (!strcmp(items[i].name, "CostFinalize")) costed = true;
        if (!cond(in, items[i].condition)) { logf(in, "Action %s skipped (condition)", items[i].name); continue; }
        logf(in, "Action: %s", items[i].name);
        ui_action(in, items[i].name);
        ok = run_action(in, items[i].name);
        if (ok && ui_cancelled(in)) { fail(in, MSI_ERROR_USEREXIT, "Cancelled by the user"); ok = false; }
    }
    in->sequence_depth--;
    if (in->sequence_depth == 0) in->stop_sequence = false;
    if (ok && execute && !costed && !in->result) { fail(in, MSI_ERROR_PACKAGE_INVALID, "The package has no CostFinalize action"); ok = false; }
    for (int i = 0; i < n; i++) free(items[i].condition);
    free(items);
    return ok;
}

/* The dialog the UI sequence names for an outcome: -1 success, -2 the
 * user cancelled, -3 failure */
static void show_exit_dialog(Inst *in, int seq)
{
    MsiTable *t = msidb_table(&in->db, "InstallUISequence");
    char b[16];
    for (int r = 0; t && r < t->nrows; r++) {
        if (msidb_int(&in->db, t, r, 2, NULL) != seq) continue;
        const char *name = msidb_str(&in->db, t, r, 0, b);
        if (!cond(in, msidb_str(&in->db, t, r, 1, b))) continue;
        char dname[80];
        snprintf(dname, sizeof(dname), "%s", name);
        if (dlg_exists(in->dlg, dname)) dlg_run(in->dlg, dname);
        else run_custom_action(in, dname);
        return;
    }
}

/* -----------------------------------------------------------------------
 * The session, for the handle API, custom actions and dialogs
 * ----------------------------------------------------------------------- */
const char *eng_get_prop(Inst *in, const char *name) { return get_prop(in, name); }

void eng_set_prop(Inst *in, const char *name, const char *value)
{
    set_prop(in, name, value);
    if (in->dlg) dlg_property_changed(in->dlg, name);
}

MsiDb *eng_db(Inst *in) { return &in->db; }
bool eng_condition(Inst *in, const char *c) { return cond(in, c); }
void eng_format(Inst *in, const MsiRec *rec, const char *s, char *out, int cap) { format_rec(in, rec, s, out, cap); }

void eng_log(Inst *in, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    logf(in, "%s", line);
}

static int dir_row(Inst *in, const char *folder)
{
    return in->dir ? msidb_find(&in->db, in->dir, 0, folder, 0) : -1;
}

int eng_target_path(Inst *in, const char *folder, char *out, int cap)
{
    if (dir_row(in, folder) < 0 && !prop_set(in, folder)) return 267;   /* ERROR_DIRECTORY */
    dir_path(in, folder, out, cap);
    return 0;
}

int eng_source_path(Inst *in, const char *folder, char *out, int cap)
{
    if (dir_row(in, folder) < 0 && strcmp(folder, "SourceDir") && strcmp(folder, "SOURCEDIR")) return 267;
    snprintf(out, (size_t)cap, "%s", in->source_dir);
    return 0;
}

int eng_set_target_path(Inst *in, const char *folder, const char *path)
{
    char p[MAX_PATH * 2];
    snprintf(p, sizeof(p), "%s", path);
    ensure_slash(p, sizeof(p));
    set_prop(in, folder, p);
    int r = dir_row(in, folder);
    if (r >= 0 && in->dir_explicit) in->dir_explicit[r] = true;
    if (in->resolved) resolve_directories(in);
    if (in->dlg) dlg_property_changed(in->dlg, folder);
    return 0;
}

int eng_do_action(Inst *in, const char *action)
{
    logf(in, "Action (requested): %s", action);
    if (run_action(in, action)) return 0;
    return in->result == MSI_ERROR_USEREXIT ? MSI_ERROR_USEREXIT : MSI_ERROR_FAILURE;
}

int eng_sequence(Inst *in, const char *table) { return run_sequence(in, table) ? 0 : MSI_ERROR_FAILURE; }

bool eng_get_mode(Inst *in, int mode)
{
    switch (mode) {
    case RUNMODE_MAINTENANCE:     return in->installed;
    case RUNMODE_ROLLBACKENABLED: return !prop_set(in, "DISABLEROLLBACK");
    case RUNMODE_LOGENABLED:      return in->log != NULL;
    case RUNMODE_CABINET:         return in->media != NULL;
    default:                      return mode >= 0 && mode < 32 && in->modes[mode];
    }
}

int eng_set_mode(Inst *in, int mode, bool state)
{
    if (mode != RUNMODE_REBOOTATEND && mode != RUNMODE_REBOOTNOW) return ERROR_ACCESS_DENIED;
    in->modes[mode] = state;
    return 0;
}

static bool comp_in_installed_feature(Inst *in, const char *comp)
{
    MsiTable *fc = msidb_table(&in->db, "FeatureComponents");
    char b[16];
    for (int r = 0; fc && r < fc->nrows; r++)
        if (!strcmp(msidb_str(&in->db, fc, r, 1, b), comp) && feature_was_installed(in, msidb_str(&in->db, fc, r, 0, b)))
            return true;
    return false;
}

int eng_feature_state(Inst *in, const char *feature, int *installed, int *action)
{
    int f = find_feature(in, feature);
    if (f < 0) return 1606;                                   /* ERROR_UNKNOWN_FEATURE */
    bool was = feature_was_installed(in, feature);
    *installed = was ? ISTATE_LOCAL : ISTATE_ABSENT;
    if (!in->feature_on) *action = ISTATE_UNKNOWN;
    else if (in->remove) *action = was ? ISTATE_ABSENT : ISTATE_UNKNOWN;
    else if (in->feature_on[f]) *action = ISTATE_LOCAL;
    else *action = was ? ISTATE_ABSENT : ISTATE_UNKNOWN;
    return 0;
}

int eng_set_feature_state(Inst *in, const char *feature, int state)
{
    int f = find_feature(in, feature);
    if (f < 0) return 1606;
    if (!in->feature_on) return 1609;                          /* before costing */
    in->feature_on[f] = state == ISTATE_LOCAL || state == ISTATE_SOURCE || state == ISTATE_DEFAULT || state == ISTATE_ADVERTISED;
    compute_components(in);
    logf(in, "Feature %s: %s", feature, in->feature_on[f] ? "install" : "do not install");
    return 0;
}

int eng_component_state(Inst *in, const char *comp, int *installed, int *action)
{
    int c = in->component ? msidb_find(&in->db, in->component, 0, comp, 0) : -1;
    if (c < 0) return 1607;                                    /* ERROR_UNKNOWN_COMPONENT */
    bool was = comp_in_installed_feature(in, comp);
    *installed = was ? ISTATE_LOCAL : ISTATE_ABSENT;
    if (!in->comp_on) *action = ISTATE_UNKNOWN;
    else if (in->remove) *action = was ? ISTATE_ABSENT : ISTATE_UNKNOWN;
    else if (in->comp_on[c]) *action = ISTATE_LOCAL;
    else *action = was ? ISTATE_ABSENT : ISTATE_UNKNOWN;
    return 0;
}

int eng_set_component_state(Inst *in, const char *comp, int state)
{
    int c = in->component ? msidb_find(&in->db, in->component, 0, comp, 0) : -1;
    if (c < 0) return 1607;
    if (!in->comp_on) return 1609;
    in->comp_on[c] = state == ISTATE_LOCAL || state == ISTATE_SOURCE || state == ISTATE_DEFAULT;
    return 0;
}

int eng_set_install_level(Inst *in, int level)
{
    char v[16];
    snprintf(v, sizeof(v), "%d", level);
    set_prop(in, "INSTALLLEVEL", v);
    if (in->feature_on) select_features(in);
    return 0;
}

/* &Feature, !Feature, $Component, ?Component in conditions */
static const char *state_prop(Inst *in, const char *name)
{
    static char buf[16];
    int installed = ISTATE_UNKNOWN, action = ISTATE_UNKNOWN;
    if (name[0] == '&' || name[0] == '!') eng_feature_state(in, name + 1, &installed, &action);
    else eng_component_state(in, name + 1, &installed, &action);
    int v = (name[0] == '&' || name[0] == '$') ? action : installed;
    snprintf(buf, sizeof(buf), "%d", v);
    return buf;
}

/* The Error table's text for message number @n */
static const char *error_template(Inst *in, int n, char *buf, int cap)
{
    MsiTable *t = msidb_table(&in->db, "Error");
    char key[16], b[16];
    snprintf(key, sizeof(key), "%d", n);
    int r = t ? msidb_find(&in->db, t, 0, key, 0) : -1;
    if (r < 0) return NULL;
    snprintf(buf, (size_t)cap, "%s", msidb_str(&in->db, t, r, 1, b));
    return buf;
}

int eng_message(Inst *in, int type, const MsiRec *rec)
{
    int kind = type & 0xFF000000;
    char text[2048] = "";
    if (rec) {
        char tb[1024], nb[16];
        const char *tmpl = NULL;
        if (!msirec_is_null(rec, 0)) tmpl = msirec_str(rec, 0, nb);
        else if ((kind == IMSG_ERROR || kind == IMSG_WARNING || kind == IMSG_USER || kind == IMSG_FATALEXIT ||
                  kind == IMSG_INFO) && rec->f[1].type == MSIF_INT)
            tmpl = error_template(in, rec->f[1].i, tb, sizeof(tb));
        if (kind == IMSG_ACTIONDATA && !tmpl && in->action_template[0]) tmpl = in->action_template;
        if (tmpl) format_rec(in, rec, tmpl, text, sizeof(text));
        else {
            size_t n = 0;
            for (int i = 1; i <= rec->n && n < sizeof(text) - 32; i++)
                n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%d: %s", i > 1 ? " " : "", i, msirec_str(rec, i, nb));
        }
    }
    switch (kind) {
    case IMSG_INFO:
        logf(in, "%s", text);
        return IDOK;
    case IMSG_ACTIONSTART: {
        char nb[16], name[128], desc[512];
        snprintf(name, sizeof(name), "%s", rec ? msirec_str(rec, 1, nb) : "");
        snprintf(desc, sizeof(desc), "%s", rec ? msirec_str(rec, 2, nb) : "");
        snprintf(in->action_template, sizeof(in->action_template), "%s", rec ? msirec_str(rec, 3, nb) : "");
        logf(in, "Action start: %s %s", name, desc);
        if (desc[0]) ui_action_text(in, desc);
        return ui_cancelled(in) ? IDCANCEL : IDOK;
    }
    case IMSG_ACTIONDATA:
        if (text[0]) ui_detail(in, text);
        return ui_cancelled(in) ? IDCANCEL : IDOK;
    case IMSG_PROGRESS: {
        int f1 = rec ? msirec_int(rec, 1) : 0, f2 = rec ? msirec_int(rec, 2) : 0;
        if (f2 == MSI_NULL_INTEGER) f2 = 0;
        if (f1 == 0) { in->prog_total = f2 > 0 ? f2 : 0; in->prog_done = 0; }
        else if (f1 == 2) in->prog_done += f2;
        else if (f1 == 3) in->prog_total += f2;
        if (in->prog_total > 0 && (f1 == 2 || f1 == 0)) {
            long long d = in->prog_done > in->prog_total ? in->prog_total : in->prog_done;
            ui_progress(in, (int)(d * 1000 / in->prog_total), 1000);
        }
        return ui_cancelled(in) ? IDCANCEL : IDOK;
    }
    case IMSG_ERROR: case IMSG_WARNING: case IMSG_USER: case IMSG_FATALEXIT: case IMSG_OUTOFDISKSPACE: case IMSG_FILESINUSE: {
        logf(in, "Message (%s): %s", kind == IMSG_ERROR ? "error" : kind == IMSG_WARNING ? "warning" : "user", text);
        UINT buttons = (UINT)type & 0xFFFF;
        if (in->ui_level <= MSIUI_BASIC || (!in->ui && !in->dlg)) return (buttons & 0xF) == MB_OK ? IDOK : 0;
        if (!(buttons & 0xF0)) buttons |= kind == IMSG_ERROR ? MB_ICONERROR : kind == IMSG_WARNING ? MB_ICONWARNING : MB_ICONINFORMATION;
        WCHAR wt[2048], wc[256];
        to_w(text, wt, 2048);
        to_w(get_prop(in, "ProductName"), wc, 256);
        return MessageBoxW(in->dlg ? dlg_window(in->dlg) : NULL, wt, wc[0] ? wc : L"Windows Installer", buttons);
    }
    case IMSG_COMMONDATA: case IMSG_INITIALIZE: case IMSG_TERMINATE: case IMSG_SHOWDIALOG: case IMSG_RESOLVESOURCE:
        return 0;
    default:
        return 0;
    }
}

/* -----------------------------------------------------------------------
 * Transforms and patches, applied to the tables before anything reads
 * them.  TRANSFORMS lists .mst files (beside the package unless the path
 * is full; ":Name" is a transform stored inside the package).  A patch
 * (.msp, from PATCH or msiexec /p) names the products it fits in its
 * summary (Template), and the transform pairs it carries (LastAuthor:
 * ":T1;:#T1"): the first changes the product, the "#" one adds the
 * patch's Media row, whose cabinet is a stream of the patch.  Patches that
 * ship whole files work; binary deltas (the Patch table, mspatcha) are
 * refused before anything changes.  What was applied is recorded with the
 * product, from copies under C:\Windows\Installer, so repairs, later
 * patches and the removal see the same tables.
 * ----------------------------------------------------------------------- */
#define MSITRANSFORM_VALIDATE_PRODUCT     0x0002
#define MSITRANSFORM_VALIDATE_UPGRADECODE 0x0800
#define ERROR_PATCH_TARGET_NOT_FOUND      1642
#define ERROR_INSTALL_TRANSFORM_FAILURE   1624
#define ERROR_PATCH_PACKAGE_INVALID       1636

/* A value of the command line's properties (before the Inst has them) */
static void cmdline_value(const WCHAR *cmd, const char *name, char *out, int cap)
{
    Inst tmp;
    memset(&tmp, 0, sizeof(tmp));
    parse_cmdline_props(&tmp, cmd);
    snprintf(out, (size_t)cap, "%s", get_prop(&tmp, name));
    for (int i = 0; i < tmp.nprops; i++) { free(tmp.props[i].name); free(tmp.props[i].value); }
    free(tmp.props);
}

/* A value the product's registration keeps (Transforms, Patches) */
static void registered_value(const char *code, const WCHAR *value, char *out, int cap)
{
    WCHAR key[300], wc[64], buf[4096];
    out[0] = 0;
    to_w(code, wc, 64);
    _snwprintf(key, 300, L"%s\\%s", NOVA_INSTALLER_KEY, wc);
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h)) return;
    DWORD type, size = sizeof(buf) - 2;
    if (!RegQueryValueExW(h, value, NULL, &type, (BYTE *)buf, &size)) { buf[size / 2] = 0; to_u8(buf, out, cap); }
    RegCloseKey(h);
}

static void list_add(char **list, const char *item)
{
    size_t n = *list ? strlen(*list) : 0;
    char *nl = realloc(*list, n + strlen(item) + 2);
    if (!nl) return;
    snprintf(nl + n, strlen(item) + 2, "%s%s", n ? ";" : "", item);
    *list = nl;
}

static MsiFile *load_msifile(const char *path)
{
    WCHAR w[MAX_PATH];
    to_w(path, w, MAX_PATH);
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL), rd = 0;
    void *data = malloc(sz ? sz : 1);
    if (!data || !ReadFile(h, data, sz, &rd, NULL) || rd != sz) { CloseHandle(h); free(data); return NULL; }
    CloseHandle(h);
    return msifile_load(data, sz);
}

/* A Property table value of the package as it stands */
static void table_prop(Inst *in, const char *name, char *out, int cap)
{
    MsiTable *pt = msidb_table(&in->db, "Property");
    int r = pt ? msidb_find(&in->db, pt, 0, name, 0) : -1;
    char b[16];
    snprintf(out, (size_t)cap, "%s", r >= 0 ? msidb_str(&in->db, pt, r, 1, b) : "");
}

/* Does the transform in @c fit this package?  Its validation flags (the
 * high word of Characters) say what to compare; Revision Number holds
 * "{product}version;{product}version;{upgrade code}" */
static bool transform_fits(Inst *in, const Cfb *c, int *suppress, char *why, int cap)
{
    int flags = 0;
    msi_suminfo_get(c, 16 /* PID_CHARCOUNT */, NULL, 0, &flags);
    *suppress = flags & 0xFFFF;
    int validate = (flags >> 16) & 0xFFFF;
    char rev[512], code[64], upgrade[64];
    msi_suminfo_get(c, 9 /* PID_REVNUMBER */, rev, sizeof(rev), NULL);
    table_prop(in, "ProductCode", code, sizeof(code));
    table_prop(in, "UpgradeCode", upgrade, sizeof(upgrade));
    if ((validate & MSITRANSFORM_VALIDATE_PRODUCT) && _strnicmp(rev, code, 38)) {
        snprintf(why, (size_t)cap, "it is for product %.38s, not %s", rev, code);
        return false;
    }
    if (validate & MSITRANSFORM_VALIDATE_UPGRADECODE) {
        const char *last = strrchr(rev, ';');
        if (!last || _strnicmp(last + 1, upgrade, 38)) { snprintf(why, (size_t)cap, "its upgrade code is not %s", upgrade); return false; }
    }
    return true;
}

static bool apply_transform(Inst *in, MsiFile *f, const char *storage, const char *what, bool must_fit)
{
    Cfb view;
    const Cfb *c = msifile_cfb(f);
    if (storage) {
        if (!cfb_open_storage(c, storage, &view)) { fail(in, ERROR_INSTALL_TRANSFORM_FAILURE, "%s: no transform %s in it", what, storage); return false; }
        c = &view;
    }
    int suppress;
    char why[256], err[256];
    if (!transform_fits(in, c, &suppress, why, sizeof(why))) {
        if (must_fit) fail(in, ERROR_INSTALL_TRANSFORM_FAILURE, "The transform %s does not fit this package: %s", what, why);
        else logf(in, "Transform %s%s%s skipped: %s", what, storage ? ":" : "", storage ? storage : "", why);
        return false;
    }
    int rc = msidb_apply_transform(&in->db, f, storage, suppress, err, sizeof(err));
    if (rc) { fail(in, rc, "The transform %s could not be applied: %s", what, err); return false; }
    logf(in, "Applied transform %s%s%s", what, storage ? ":" : "", storage ? storage : "");
    return true;
}

/* The path of a transform or patch named on the command line */
static void source_relative(Inst *in, const char *spec, char *out, int cap)
{
    if (spec[0] == '@' || spec[0] == '|') spec++;          /* secure-at-source, secure-full-path */
    if (spec[0] && (spec[1] == ':' || spec[0] == '\\')) snprintf(out, (size_t)cap, "%s", spec);
    else snprintf(out, (size_t)cap, "%s%s", in->source_dir, spec);
}

static bool apply_transforms(Inst *in, const char *list)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", list);
    char *save = NULL;
    for (char *p = strtok_s(buf, ";", &save); p; p = strtok_s(NULL, ";", &save)) {
        while (*p == ' ') p++;
        if (!*p) continue;
        MsiFile *f;
        const char *storage = NULL;
        char path[MAX_PATH];
        if (p[0] == ':') {                                   /* inside the package */
            void *copy = malloc(in->pkg_size ? in->pkg_size : 1);
            if (!copy) { fail(in, MSI_ERROR_FAILURE, "Out of memory"); return false; }
            memcpy(copy, in->pkg, in->pkg_size);
            f = msifile_load(copy, in->pkg_size);
            storage = p + 1;
            snprintf(path, sizeof(path), "%s", p);
        } else {
            source_relative(in, p, path, sizeof(path));
            f = load_msifile(path);
        }
        if (!f) { fail(in, ERROR_INSTALL_TRANSFORM_FAILURE, "The transform %s could not be opened", path); return false; }
        bool ok = apply_transform(in, f, storage, path, true);
        msifile_release(f);
        if (!ok) return false;
        list_add(&in->xforms, path);
    }
    return true;
}

/* A patch: does it fit, which transforms, then its cabinets */
static bool apply_patch(Inst *in, const char *path)
{
    MsiFile *f = load_msifile(path);
    if (!f) { fail(in, ERROR_PATCH_PACKAGE_INVALID, "The patch %s could not be opened", path); return false; }
    const Cfb *c = msifile_cfb(f);
    char targets[2048], xforms[1024], code[64], pcode[64];
    msi_suminfo_get(c, 7 /* PID_TEMPLATE */, targets, sizeof(targets), NULL);
    msi_suminfo_get(c, 8 /* PID_LASTAUTHOR */, xforms, sizeof(xforms), NULL);
    msi_suminfo_get(c, 9 /* PID_REVNUMBER */, pcode, sizeof(pcode), NULL);
    pcode[38] = 0;
    table_prop(in, "ProductCode", code, sizeof(code));
    if (in->patch_remove[0] && !_stricmp(in->patch_remove, pcode)) {
        logf(in, "Patch %s (%s) is being removed: not applied", pcode, path);
        snprintf(in->removed_patch, sizeof(in->removed_patch), "%s", path);
        msifile_release(f);
        return true;
    }
    if (!strstr(targets, code)) {
        fail(in, ERROR_PATCH_TARGET_NOT_FOUND, "The patch %s is not for %s (it is for %s)", path, code, targets);
        msifile_release(f);
        return false;
    }
    MsiTable *pt = msidb_table(&in->db, "Patch");
    int deltas_before = pt ? pt->nrows : 0;
    int applied = 0;
    char list[1024];
    snprintf(list, sizeof(list), "%s", xforms);
    char *save = NULL;
    for (char *p = strtok_s(list, ";", &save); p; p = strtok_s(NULL, ";", &save)) {
        if (*p == ':') p++;
        if (*p == '#' || !*p) continue;                      /* (applied with its partner) */
        char partner[128];
        snprintf(partner, sizeof(partner), "#%s", p);
        if (!apply_transform(in, f, p, path, false)) {
            if (in->result) { msifile_release(f); return false; }
            continue;
        }
        if (!apply_transform(in, f, partner, path, false) && in->result) { msifile_release(f); return false; }
        applied++;
    }
    if (!applied) {
        fail(in, ERROR_PATCH_TARGET_NOT_FOUND, "None of the transforms in the patch %s fits %s %s", path, code, get_prop(in, "ProductVersion"));
        msifile_release(f);
        return false;
    }
    pt = msidb_table(&in->db, "Patch");
    if (pt && pt->nrows > deltas_before) {
        fail(in, MSI_ERROR_FAILURE, "The patch %s changes files with binary deltas (its Patch table), which NovaOS cannot apply yet; "
             "a patch that ships whole files works", path);
        msifile_release(f);
        return false;
    }
    msidb_add_streams(&in->db, f, NULL);                     /* its cabinets */
    msifile_release(f);
    logf(in, "Applied patch %s (%s)", pcode, path);
    list_add(&in->patches, path);
    return true;
}

static bool apply_patches(Inst *in, const char *list)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", list);
    char *save = NULL;
    for (char *p = strtok_s(buf, ";", &save); p; p = strtok_s(NULL, ";", &save)) {
        while (*p == ' ') p++;
        if (!*p) continue;
        char path[MAX_PATH];
        source_relative(in, p, path, sizeof(path));
        if (in->patches && strstr(in->patches, path)) continue;  /* registered and named again */
        if (!apply_patch(in, path)) return false;
    }
    return true;
}

/* What changes the tables: the product's recorded transforms and patches
 * when it is installed, then those the command line names */
static bool apply_changes(Inst *in)
{
    char code[64], xf[4096], pa[4096], reg_xf[4096], reg_pa[4096];
    table_prop(in, "ProductCode", code, sizeof(code));
    cmdline_value(in->cmdline, "TRANSFORMS", xf, sizeof(xf));
    cmdline_value(in->cmdline, "PATCH", pa, sizeof(pa));
    cmdline_value(in->cmdline, "MSIPATCHREMOVE", in->patch_remove, sizeof(in->patch_remove));
    registered_value(code, L"Transforms", reg_xf, sizeof(reg_xf));
    registered_value(code, L"Patches", reg_pa, sizeof(reg_pa));
    if (!apply_transforms(in, reg_xf[0] ? reg_xf : xf)) return false;
    if (!apply_patches(in, reg_pa) || !apply_patches(in, pa)) return false;
    /* the patches' property */
    if (in->patches) set_prop(in, "PATCH", in->patches);
    return true;
}

/* At RegisterProduct: copies of the transforms and patches under
 * C:\Windows\Installer, and their list with the product */
static void cache_changes(Inst *in, HKEY h)
{
    const char *code = get_prop(in, "ProductCode");
    char dir[MAX_PATH], kept[4096];
    snprintf(dir, sizeof(dir), "C:\\Windows\\Installer\\%s\\", code);
    for (int pass = 0; pass < 2; pass++) {
        const char *list = pass ? in->patches : in->xforms;
        kept[0] = 0;
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s", list ? list : "");
        char *save = NULL;
        for (char *p = strtok_s(buf, ";", &save); p; p = strtok_s(NULL, ";", &save)) {
            char cached[MAX_PATH];
            if (!_strnicmp(p, "C:\\Windows\\Installer\\", 21) || p[0] == ':') snprintf(cached, sizeof(cached), "%s", p);
            else {
                const char *base = strrchr(p, '\\');
                make_dirs(in, dir);
                snprintf(cached, sizeof(cached), "%s%s", dir, base ? base + 1 : p);
                WCHAR ws[MAX_PATH], wd[MAX_PATH];
                to_w(p, ws, MAX_PATH);
                to_w(cached, wd, MAX_PATH);
                rb_file(in, wd);
                if (!CopyFileW(ws, wd, FALSE)) { logf(in, "Could not keep a copy of %s", p); continue; }

            }
            size_t n = strlen(kept);
            snprintf(kept + n, sizeof(kept) - n, "%s%s", n ? ";" : "", cached);
        }
        set_sz(h, pass ? L"Patches" : L"Transforms", kept);
    }
    if (in->removed_patch[0] && !_strnicmp(in->removed_patch, "C:\\Windows\\Installer\\", 21)) {
        WCHAR w[MAX_PATH];
        to_w(in->removed_patch, w, MAX_PATH);
        rb_delete_file(in, w);
        logf(in, "Patch %s removed from the product", in->patch_remove);
    }

}

/* -----------------------------------------------------------------------
 * Entry points
 * ----------------------------------------------------------------------- */
static bool load_package(Inst *in, const WCHAR *path)
{
    WCHAR full[MAX_PATH];
    if (!GetFullPathNameW(path, MAX_PATH, full, NULL)) wcsncpy(full, path, MAX_PATH);
    wcsncpy(in->pkg_path, full, MAX_PATH);
    HANDLE h = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { fail(in, MSI_ERROR_PACKAGE_OPEN, "The package could not be opened"); return false; }
    DWORD sz = GetFileSize(h, NULL), rd = 0;
    in->pkg = malloc(sz ? sz : 1);
    if (!in->pkg || !ReadFile(h, in->pkg, sz, &rd, NULL) || rd != sz) { CloseHandle(h); fail(in, MSI_ERROR_PACKAGE_OPEN, "The package could not be read"); return false; }
    CloseHandle(h);
    in->pkg_size = sz;
    if (!msidb_open(&in->db, in->pkg, in->pkg_size)) { fail(in, MSI_ERROR_PACKAGE_INVALID, "This is not a Windows Installer package"); return false; }
    char u8[MAX_PATH];
    to_u8(full, u8, sizeof(u8));
    snprintf(in->source_dir, sizeof(in->source_dir), "%s", u8);
    char *bs = strrchr(in->source_dir, '\\');
    if (bs) bs[1] = 0;
    if (!apply_changes(in)) return false;                   /* transforms and patches */
    in->feature   = msidb_table(&in->db, "Feature");
    in->component = msidb_table(&in->db, "Component");
    in->file      = msidb_table(&in->db, "File");
    in->dir       = msidb_table(&in->db, "Directory");
    in->media     = msidb_table(&in->db, "Media");
    /* the Property table */
    MsiTable *pt = msidb_table(&in->db, "Property");
    char b[16];
    for (int r = 0; pt && r < pt->nrows; r++)
        set_prop(in, msidb_str(&in->db, pt, r, 0, b), msidb_str(&in->db, pt, r, 1, b));
    return true;
}

static void inst_free(Inst *in)
{
    if (in->nrb) { if (in->result) rb_run(in); else rb_commit(in); }
    for (int i = 0; i < in->nprops; i++) { free(in->props[i].name); free(in->props[i].value); }
    free(in->props);
    free(in->feature_on);
    free(in->comp_on);
    free(in->dir_explicit);
    free(in->installed_features);
    free(in->xforms);
    free(in->patches);
    for (int i = 0; i < in->nran; i++) free(in->ran[i]);
    free(in->ran);
    for (int i = 0; i < in->ncommit; i++) free(in->commit[i]);
    free(in->commit);
    msisql_free_temp(&in->db);
    msidb_close(&in->db);
    free(in->pkg);
    if (in->log) fclose(in->log);
}

/* The features recorded at install time (",A,B,") */
static void read_installed_features(Inst *in, const char *code)
{
    WCHAR key[300], wc[64], buf[4096];
    to_w(code, wc, 64);
    _snwprintf(key, 300, L"%s\\%s", NOVA_INSTALLER_KEY, wc);
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h)) return;
    DWORD type, size = sizeof(buf) - 2;
    if (!RegQueryValueExW(h, L"Features", NULL, &type, (BYTE *)buf, &size)) {
        buf[size / 2] = 0;
        char u8[4096];
        to_u8(buf, u8, sizeof(u8));
        in->installed_features = strdup(u8);
    }
    RegCloseKey(h);
}

static int run(Inst *in, const MsiRequest *req, bool *dialogs_shown)
{
    if (req->logfile) {
        in->log = _wfopen(req->logfile, L"wb");
        if (!in->log) logf(in, "Could not open the log file");
    }
    in->cmdline = req->properties;
    if (!load_package(in, in->pkg_path)) return in->result;
    /* properties: package, system, command line (which wins) */
    system_properties(in);
    parse_cmdline_props(in, req->properties);
    const char *code = get_prop(in, "ProductCode");
    in->installed = product_registered(code, NULL, 0);
    if (in->installed) {
        set_prop(in, "Installed", "1");
        set_prop(in, "ProductState", "5");
        read_installed_features(in, code);
    } else set_prop(in, "ProductState", "-1");
    if (!strcmp(get_prop(in, "REMOVE"), "ALL")) in->remove = true;
    if (in->remove) {
        if (!in->installed) logf(in, "Product %s is not registered; removing what the package lists anyway", code);
        else restore_install_location(in, code);
        set_prop(in, "REMOVE", "ALL");
    } else if (in->installed && in->ui_level < MSIUI_FULL) {
        set_prop(in, "REINSTALL", "ALL");
        set_prop(in, "REINSTALLMODE", "vomus");
    }
    if (in->installed && !in->remove) restore_install_location(in, code);
    logf(in, "%s %s %s (%s)", in->remove ? "Removing" : "Installing", get_prop(in, "ProductName"),
         get_prop(in, "ProductVersion"), code);
    /* full UI and a package with dialogs: its own InstallUISequence */
    if (in->ui_level >= MSIUI_FULL && !in->remove && dlg_available(&in->db)) {
        in->dlg = dlg_create(in);
        if (in->dlg) {
            *dialogs_shown = true;
            in->in_ui = true;
            bool ok = run_sequence(in, "InstallUISequence");
            if (ok && !in->executed && !in->result) ok = action_execute(in);
            in->in_ui = false;
            int outcome = !in->result ? -1 : in->result == MSI_ERROR_USEREXIT ? -2 : -3;
            if (outcome == -3) eng_set_prop(in, "ERRORMESSAGE", in->error);   /* (shown by some FatalError dialogs) */
            show_exit_dialog(in, outcome);
            dlg_destroy(in->dlg);
            in->dlg = NULL;
            if (!in->result) logf(in, "Installation completed successfully");
            return in->result;
        }
    }
    if (in->ui) {
        char title[256];
        snprintf(title, sizeof(title), "%s", get_prop(in, "ProductName"));
        msiui_begin(in->ui, title, in->remove);
    }
    execute_sequence(in);

    if (!in->result) logf(in, "%s completed successfully", in->remove ? "Removal" : "Installation");
    return in->result;
}

static int run_product(const WCHAR *code, bool remove, int ui_level, MsiUi *ui, const WCHAR *props, char *err, int cap)
{
    char u8[64];
    to_u8(code, u8, sizeof(u8));
    WCHAR pkg[MAX_PATH];
    if (!product_registered(u8, pkg, MAX_PATH) || !pkg[0]) {
        if (err) snprintf(err, (size_t)cap, "Product %s is not installed", u8);
        return MSI_ERROR_UNKNOWN_PRODUCT;
    }
    MsiRequest req;
    memset(&req, 0, sizeof(req));
    req.package = pkg;
    req.remove = remove;
    req.ui_level = ui_level;
    req.properties = props;
    Inst *in = calloc(1, sizeof(Inst));
    in->remove = remove;
    in->ui_level = ui_level;
    in->ui = ui;
    wcsncpy(in->pkg_path, pkg, MAX_PATH);
    bool shown = false;
    int r = run(in, &req, &shown);
    if (r && err) snprintf(err, (size_t)cap, "%s", in->error);
    inst_free(in);
    free(in);
    return r;
}

/* msiexec /p (apply a patch to the installed product it names) and
 * /uninstall patch.msp (take it off again): a repair of the product with
 * PATCH or MSIPATCHREMOVE */
static int run_patch(const MsiRequest *req, MsiUi *ui, char *err, int cap)
{
    WCHAR full[MAX_PATH];
    if (!GetFullPathNameW(req->patch, MAX_PATH, full, NULL)) wcsncpy(full, req->patch, MAX_PATH);
    char path[MAX_PATH], targets[2048] = "", pcode[64] = "";
    to_u8(full, path, sizeof(path));
    MsiFile *f = load_msifile(path);
    if (!f) { snprintf(err, (size_t)cap, "The patch %s could not be opened", path); return ERROR_PATCH_PACKAGE_INVALID; }
    msi_suminfo_get(msifile_cfb(f), 7, targets, sizeof(targets), NULL);
    msi_suminfo_get(msifile_cfb(f), 9, pcode, sizeof(pcode), NULL);
    msifile_release(f);
    pcode[38] = 0;
    /* the first of its target products that is installed */
    char code[64] = "";
    for (const char *p = targets; *p; ) {
        const char *e = strchr(p, ';');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char one[64];
        snprintf(one, sizeof(one), "%.*s", (int)(l < 63 ? l : 63), p);
        if (one[0] && product_registered(one, NULL, 0)) { snprintf(code, sizeof(code), "%s", one); break; }
        if (!e) break;
        p = e + 1;
    }
    if (!code[0]) { snprintf(err, (size_t)cap, "None of the products this patch updates (%s) is installed", targets); return ERROR_PATCH_TARGET_NOT_FOUND; }
    WCHAR props[4096 + MAX_PATH], wcode[64], wpcode[64];
    to_w(pcode, wpcode, 64);
    if (req->remove) _snwprintf(props, 4096 + MAX_PATH, L"MSIPATCHREMOVE=%s REINSTALL=ALL REINSTALLMODE=omus %s", wpcode, req->properties ? req->properties : L"");

    else _snwprintf(props, 4096 + MAX_PATH, L"PATCH=\"%s\" REINSTALL=ALL REINSTALLMODE=omus %s", full, req->properties ? req->properties : L"");
    to_w(code, wcode, 64);
    return run_product(wcode, false, req->ui_level > MSIUI_BASIC ? MSIUI_BASIC : req->ui_level, ui, props, err, cap);
}

int MsiRunInstall(const MsiRequest *req, char *err, int err_cap)
{
    if (err && err_cap) err[0] = 0;
    MsiUi *ui = req->ui_level > MSIUI_NONE ? msiui_create() : NULL;
    int r;
    bool dialogs = false;
    if (req->patch && !req->package && !req->product_code) {
        r = run_patch(req, ui, err, err_cap);
    } else if (req->product_code && !req->package) {
        r = run_product(req->product_code, req->remove, req->ui_level, ui, req->properties, err, err_cap);
    } else {
        Inst *in = calloc(1, sizeof(Inst));
        in->remove = req->remove;
        in->ui_level = req->ui_level;
        in->ui = ui;
        wcsncpy(in->pkg_path, req->package ? req->package : L"", MAX_PATH);
        r = run(in, req, &dialogs);
        if (r && err) snprintf(err, (size_t)err_cap, "%s", in->error);
        inst_free(in);
        free(in);
    }
    /* the package's own dialogs already told the user how it went */
    if (ui) msiui_end(ui, dialogs ? MSI_OK : r, req->ui_level >= MSIUI_FULL && !dialogs, err);
    return r;
}

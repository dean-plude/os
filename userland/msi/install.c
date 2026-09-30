/*
 * install.c — the installation engine
 *
 * Runs the package's InstallExecuteSequence: the costing actions select
 * features and components and resolve the Directory table to paths, then
 * InstallFiles extracts the cabinets, WriteRegistryValues writes the
 * Registry table, and InstallFinalize registers the product under the
 * Uninstall key and keeps a copy of the package for uninstalling.
 * Custom actions that set properties or directories (types 51 and 35)
 * run; ones that execute code are logged and skipped.  Uninstalling
 * (REMOVE=ALL) reverses the files, folders and registry entries.
 */
#define MSI_EXPORT __declspec(dllexport)
#include "msi.h"
#include "msi_int.h"
#include "ui.h"

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
typedef struct { char *name, *value; } Prop;

typedef struct {
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
    bool      removed_files, removed_registry, removed_folders;
} Inst;

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

static const char *cond_prop(void *ctx, const char *name) { return get_prop(ctx, name); }
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

static void format_str(Inst *in, const char *s, char *out, int cap)
{
    int n = 0;
    while (*s && n < cap - 1) {
        if (*s != '[') { out[n++] = *s++; continue; }
        const char *e = strchr(s + 1, ']');
        if (!e) { out[n++] = *s++; continue; }
        char key[256];
        size_t kl = (size_t)(e - s - 1);
        if (kl >= sizeof(key)) kl = sizeof(key) - 1;
        memcpy(key, s + 1, kl);
        key[kl] = '\0';
        s = e + 1;
        char val[MAX_PATH * 2];
        const char *v = "";
        if (key[0] == '\\' && key[1]) { val[0] = key[1]; val[1] = 0; v = val; }
        else if (!strcmp(key, "~")) { v = ""; }                 /* null separator: dropped */
        else if (key[0] == '#') { file_path(in, key + 1, val, sizeof(val)); v = val; }
        else if (key[0] == '$') { dir_path(in, key + 1, val, sizeof(val)); v = val; }
        else if (key[0] == '%') { v = get_prop(in, key); }
        else if (key[0] == '!') { v = get_prop(in, key + 1); }
        else if (!key[0]) { v = ""; }
        else v = get_prop(in, key);
        for (; *v && n < cap - 1; v++) out[n++] = *v;
    }
    out[n] = '\0';
}

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
        { "ProgramMenuFolder", "C:\\ProgramData\\Start Menu\\Programs\\" },
        { "StartMenuFolder", "C:\\ProgramData\\Start Menu\\" },
        { "StartupFolder", "C:\\ProgramData\\Start Menu\\Programs\\Startup\\" },
        { "AppDataFolder", "C:\\AppData\\Roaming\\" }, { "LocalAppDataFolder", "C:\\AppData\\Local\\" },
        { "CommonAppDataFolder", "C:\\ProgramData\\" }, { "PersonalFolder", "C:\\Documents\\" },
        { "MyPicturesFolder", "C:\\Pictures\\" }, { "SendToFolder", "C:\\AppData\\Roaming\\SendTo\\" },
        { "TemplateFolder", "C:\\AppData\\Roaming\\Templates\\" }, { "FavoritesFolder", "C:\\Favorites\\" },
        { "NetHoodFolder", "C:\\AppData\\Roaming\\NetHood\\" }, { "PrintHoodFolder", "C:\\AppData\\Roaming\\PrintHood\\" },
        { "RecentFolder", "C:\\AppData\\Roaming\\Recent\\" }, { "AdminToolsFolder", "C:\\ProgramData\\Start Menu\\Programs\\Administrative Tools\\" },
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

static void select_features(Inst *in)
{
    MsiTable *ft = in->feature;
    int nf = ft ? ft->nrows : 0;
    in->feature_on = calloc((size_t)nf + 1, 1);
    int level = atoi(get_prop(in, "INSTALLLEVEL"));
    if (level <= 0) level = 1;
    /* the Condition table changes feature levels */
    MsiTable *ct = msidb_table(&in->db, "Condition");
    char b[16];
    for (int i = 0; i < nf; i++) {
        const char *name = msidb_str(&in->db, ft, i, 0, b);
        int lv = msidb_int(&in->db, ft, i, msidb_col(ft, "Level"), NULL);
        for (int r = 0; ct && r < ct->nrows; r++) {
            if (strcmp(msidb_str(&in->db, ct, r, 0, b), name)) continue;
            int nl = msidb_int(&in->db, ct, r, 1, NULL);
            if (cond(in, msidb_str(&in->db, ct, r, 2, b))) lv = nl;
        }
        bool on = lv > 0 && lv <= level;
        const char *add = get_prop(in, "ADDLOCAL"), *rem = get_prop(in, "REMOVE");
        if (in_list(add, name)) on = true;
        if (in_list(rem, name)) on = false;
        if (in->remove) on = true;             /* everything that was installed goes */
        in->feature_on[i] = on;
    }
    /* a feature whose parent is off is off */
    for (int pass = 0; pass < 8; pass++)
        for (int i = 0; i < nf; i++) {
            const char *parent = msidb_str(&in->db, ft, i, 1, b);
            if (!*parent || !in->feature_on[i]) continue;
            int p = find_feature(in, parent);
            if (p >= 0 && p != i && !in->feature_on[p]) in->feature_on[i] = false;
        }

    MsiTable *comp = in->component, *fc = msidb_table(&in->db, "FeatureComponents");
    int nc = comp ? comp->nrows : 0;
    in->comp_on = calloc((size_t)nc + 1, 1);
    int ccond = msidb_col(comp, "Condition");
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
    int fon = 0, con = 0;
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
    /* every Directory key becomes a property holding its path (ones set
     * on the command line or by custom actions keep their value) */
    char b[16], path[MAX_PATH];
    for (int r = 0; in->dir && r < in->dir->nrows; r++) {
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
 * Files
 * ----------------------------------------------------------------------- */
static bool make_dirs(const char *path)
{
    /* create every folder on the way */
    WCHAR w[MAX_PATH];
    to_w(path, w, MAX_PATH);
    for (int i = 3; w[i]; i++) {
        if (w[i] != L'\\') continue;
        w[i] = 0;
        CreateDirectoryW(w, NULL);
        w[i] = L'\\';
    }
    size_t n = wcslen(w);
    if (n && w[n - 1] != L'\\') CreateDirectoryW(w, NULL);
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

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
        oc->data = cfb_read(&in->db.cfb, cabname + 1, &oc->size);
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
    if (bs) { bs[1] = 0; make_dirs(dir); }
    WCHAR wp[MAX_PATH];
    to_w(w->pf->target, wp, MAX_PATH);
    SetFileAttributesW(wp, FILE_ATTRIBUTE_NORMAL);
    w->h = CreateFileW(wp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (w->h == INVALID_HANDLE_VALUE) {
        fail(in, MSI_ERROR_FAILURE, "Could not create %s (error %lu)", w->pf->target, GetLastError());
        return false;
    }
    if (in->ui) {
        char status[MAX_PATH + 32];
        const char *name = strrchr(w->pf->target, '\\');
        snprintf(status, sizeof(status), "Copying new files: %s", name ? name + 1 : w->pf->target);
        msiui_status(in->ui, status);
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
    if (in->ui) msiui_progress(in->ui, in->done_work, in->total_work);
}

/* Extract one cabinet set (starting at Media row @m) for the planned
 * files it holds; continues into following cabinets when a folder spans */
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
            if (!p || p->done) continue;
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
            if (in->ui && msiui_cancelled(in->ui)) { fail(in, MSI_ERROR_USEREXIT, "Cancelled by the user"); ok = false; break; }
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
    if (bs) { bs[1] = 0; make_dirs(dir); }
    logf(in, "Copying %s to %s", src, p->target);
    if (!CopyFileW(ws, wd, FALSE)) { fail(in, MSI_ERROR_PACKAGE_OPEN, "Source file %s is missing", src); return false; }
    p->done = true;
    in->done_work++;
    if (in->ui) msiui_progress(in->ui, in->done_work, in->total_work);
    return true;
}

static bool action_install_files(Inst *in)
{
    int n;
    PlanFile *pf = plan_files(in, &n);
    in->total_work = n + 1;
    if (in->ui) msiui_progress(in->ui, 0, in->total_work);
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
    for (int i = 0; ok && i < n; i++)
        if (!pf[i].done) { logf(in, "Warning: %s was not in any cabinet", pf[i].key); }
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
        SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(w)) logf(in, "Removed %s", pf[i].target);
        else if (GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES) logf(in, "Could not remove %s", pf[i].target);
        in->done_work++;
        if (in->ui) { msiui_progress(in->ui, in->done_work, in->total_work); }
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
        make_dirs(path);
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
        if (!RemoveDirectoryW(w)) return;
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
    if (RegCreateKeyExW(root_key(in, root), wkey, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &h, NULL)) {
        logf(in, "Could not create registry key %s", key);
        return;
    }
    if (!strcmp(name, "*") || !strcmp(name, "+") || !strcmp(name, "-")) { RegCloseKey(h); return; }
    const WCHAR *vname = name[0] ? wname : NULL;
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
        if (!strcmp(name, "*") || !strcmp(name, "-")) { RegDeleteKeyW(root_key(in, root), wkey); continue; }
        if (!strcmp(name, "+")) continue;
        HKEY h;
        if (RegOpenKeyExW(root_key(in, root), wkey, 0, KEY_ALL_ACCESS, &h)) continue;
        RegDeleteValueW(h, name[0] ? wname : NULL);
        RegCloseKey(h);
        RegDeleteKeyW(root_key(in, root), wkey);   /* goes only if empty */
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
    make_dirs("C:\\Windows\\Installer\\");
    WCHAR cache[MAX_PATH];
    _snwprintf(cache, MAX_PATH, L"C:\\Windows\\Installer\\%s.msi", wc);
    if (!CopyFileW(in->pkg_path, cache, FALSE)) logf(in, "Could not cache the package");
    _snwprintf(key, 300, L"%s\\%s", NOVA_INSTALLER_KEY, wc);
    if (!RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &h, NULL)) {
        RegSetValueExW(h, L"LocalPackage", 0, REG_SZ, (const BYTE *)cache, (DWORD)(wcslen(cache) + 1) * 2);
        set_sz(h, L"ProductName", get_prop(in, "ProductName"));
        set_sz(h, L"UpgradeCode", get_prop(in, "UpgradeCode"));
        set_sz(h, L"ProductVersion", get_prop(in, "ProductVersion"));
        char loc[MAX_PATH];
        install_location(in, loc, sizeof(loc));
        set_sz(h, L"InstallLocation", loc);
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
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    _snwprintf(key, 300, L"%s\\%s", NOVA_INSTALLER_KEY, wc);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    WCHAR cache[MAX_PATH];
    _snwprintf(cache, MAX_PATH, L"C:\\Windows\\Installer\\%s.msi", wc);
    DeleteFileW(cache);
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

static void action_app_search(Inst *in)
{
    /* AppSearch(Property, Signature_) with RegLocator(Signature_, Root, Key, Name, Type) */
    MsiTable *as = msidb_table(&in->db, "AppSearch"), *rl = msidb_table(&in->db, "RegLocator");
    char b[16];
    for (int r = 0; as && rl && r < as->nrows; r++) {
        const char *prop = msidb_str(&in->db, as, r, 0, b);
        char sig[80];
        snprintf(sig, sizeof(sig), "%s", msidb_str(&in->db, as, r, 1, b));
        int l = msidb_find(&in->db, rl, 0, sig, 0);
        if (l < 0) continue;
        int root = msidb_int(&in->db, rl, l, 1, NULL);
        char key[512], name[256];
        format_str(in, msidb_str(&in->db, rl, l, 2, b), key, sizeof(key));
        format_str(in, msidb_str(&in->db, rl, l, 3, b), name, sizeof(name));
        WCHAR wkey[512], wname[256], wv[1024];
        to_w(key, wkey, 512);
        to_w(name, wname, 256);
        HKEY h;
        if (RegOpenKeyExW(root_key(in, root), wkey, 0, KEY_READ, &h)) continue;
        DWORD type, size = sizeof(wv);
        if (!RegQueryValueExW(h, name[0] ? wname : NULL, NULL, &type, (BYTE *)wv, &size)) {
            char v[1024];
            if (type == REG_DWORD) snprintf(v, sizeof(v), "#%lu", (unsigned long)*(DWORD *)wv);
            else to_u8(wv, v, sizeof(v));
            char prop_name[80];
            snprintf(prop_name, sizeof(prop_name), "%s", prop);
            set_prop(in, prop_name, v);
            logf(in, "AppSearch: %s = %s", prop_name, v);
        }
        RegCloseKey(h);
    }
}

static int run_product(const WCHAR *code, bool remove, int ui_level, MsiUi *ui, char *err, int cap);

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
            run_product(wc, true, MSIUI_NONE, in->ui, err, sizeof(err));
            if (!e) break;
            p = e + 1;
        }
    }
}

static void action_shortcuts(Inst *in)
{
    MsiTable *t = msidb_table(&in->db, "Shortcut");
    char b[16];
    for (int r = 0; t && r < t->nrows; r++) {
        if (!comp_enabled(in, msidb_str(&in->db, t, r, 3, b))) continue;
        char name[MAX_PATH];
        long_name(msidb_str(&in->db, t, r, 2, b), name, sizeof(name));
        logf(in, "Shortcut \"%s\" not created (no shortcuts on NovaOS; programs start from C:\\Programs)", name);
    }
}

/* Custom actions: only the property/directory setters run */
static bool run_custom_action(Inst *in, const char *name)
{
    MsiTable *t = msidb_table(&in->db, "CustomAction");
    int r = msidb_find(&in->db, t, 0, name, 0);
    if (r < 0) { logf(in, "Action %s: unknown, skipped", name); return true; }
    char b[16];
    int type = msidb_int(&in->db, t, r, 1, NULL);
    const char *source = msidb_str(&in->db, t, r, 2, b);
    char target[1024];
    const char *raw_target = msidb_str(&in->db, t, r, 3, b);
    switch (type & 0x3F) {
    case 51:                                   /* set property */
        format_str(in, raw_target, target, sizeof(target));
        set_prop(in, source, target);
        logf(in, "Property %s = %s", source, target);
        return true;
    case 35:                                   /* set directory */
        format_str(in, raw_target, target, sizeof(target));
        ensure_slash(target, sizeof(target));
        set_prop(in, source, target);
        logf(in, "Directory %s = %s", source, target);
        return true;
    case 19:                                   /* error message */
        format_str(in, raw_target, target, sizeof(target));
        fail(in, MSI_ERROR_FAILURE, "%s", target[0] ? target : name);
        return false;
    default:
        logf(in, "Custom action %s (type %d) skipped: not run on NovaOS", name, type);
        return true;
    }
}

/* -----------------------------------------------------------------------
 * The sequence
 * ----------------------------------------------------------------------- */
static bool run_action(Inst *in, const char *a)
{
    if (!strcmp(a, "CostInitialize") || !strcmp(a, "FileCost") || !strcmp(a, "InstallValidate") ||
        !strcmp(a, "InstallInitialize") || !strcmp(a, "ProcessComponents") || !strcmp(a, "ValidateProductID") ||
        !strcmp(a, "MigrateFeatureStates") || !strcmp(a, "PublishFeatures") || !strcmp(a, "PublishProduct") ||
        !strcmp(a, "RegisterUser") || !strcmp(a, "UnpublishFeatures") || !strcmp(a, "UnpublishComponents") ||
        !strcmp(a, "RegisterComponents") || !strcmp(a, "UnregisterComponents") || !strcmp(a, "SetODBCFolders") ||
        !strcmp(a, "InstallODBC") || !strcmp(a, "RemoveODBC") || !strcmp(a, "AllocateRegistrySpace") ||
        !strcmp(a, "ScheduleReboot") || !strcmp(a, "ForceReboot") || !strcmp(a, "IsolateComponents") ||
        !strcmp(a, "RegisterProgIdInfo") || !strcmp(a, "UnregisterProgIdInfo") || !strcmp(a, "DuplicateFiles") ||
        !strcmp(a, "RemoveDuplicateFiles") || !strcmp(a, "PatchFiles") || !strcmp(a, "BindImage") ||
        !strcmp(a, "SelfRegModules") || !strcmp(a, "SelfUnregModules") || !strcmp(a, "RemoveIniValues") ||
        !strcmp(a, "WriteIniValues") || !strcmp(a, "DeleteServices") || !strcmp(a, "InstallServices") ||
        !strcmp(a, "StartServices") || !strcmp(a, "StopServices") || !strcmp(a, "RegisterFonts") ||
        !strcmp(a, "UnregisterFonts") || !strcmp(a, "RegisterTypeLibraries") || !strcmp(a, "UnregisterTypeLibraries") ||
        !strcmp(a, "RegisterExtensionInfo") || !strcmp(a, "UnregisterExtensionInfo") || !strcmp(a, "RegisterMIMEInfo") ||
        !strcmp(a, "UnregisterMIMEInfo") || !strcmp(a, "RegisterClassInfo") || !strcmp(a, "UnregisterClassInfo") ||
        !strcmp(a, "MoveFiles") || !strcmp(a, "InstallAdminPackage") || !strcmp(a, "InstallSFPCatalogFile") ||
        !strcmp(a, "RemoveEnvironmentStrings") || !strcmp(a, "WriteEnvironmentStrings") ||
        !strcmp(a, "RemoveRegistryValues") || !strcmp(a, "RemoveShortcuts") || !strcmp(a, "InstallExecute") ||
        !strcmp(a, "InstallExecuteAgain") || !strcmp(a, "RMCCPSearch") || !strcmp(a, "CCPSearch") ||
        !strcmp(a, "MsiPublishAssemblies") || !strcmp(a, "MsiUnpublishAssemblies") || !strcmp(a, "RegisterFonts") ||
        !strcmp(a, "RemoveFiles") || !strcmp(a, "RemoveFolders") || !strcmp(a, "RegisterProduct") ||
        !strcmp(a, "CreateShortcuts") || !strcmp(a, "InstallFinalize")) {
        /* the ones with work in them */
        if (!strcmp(a, "CostFinalize")) { }
        if (!strcmp(a, "RemoveRegistryValues") && in->remove) { action_remove_registry(in); in->removed_registry = true; }
        else if (!strcmp(a, "RemoveFiles") && in->remove) { action_remove_files(in); in->removed_files = true; }
        else if (!strcmp(a, "RemoveFolders") && in->remove) { action_remove_folders(in); in->removed_folders = true; }
        else if (!strcmp(a, "RegisterProduct") && !in->remove) action_register_product(in);
        else if (!strcmp(a, "CreateShortcuts") && !in->remove) action_shortcuts(in);
        else if (!strcmp(a, "InstallFinalize") && in->remove) {
            /* a package whose sequence lacks the removal actions still gets cleaned up */
            if (!in->removed_registry) action_remove_registry(in);
            if (!in->removed_files) action_remove_files(in);
            if (!in->removed_folders) action_remove_folders(in);
            action_unregister_product(in);
        }
        else if (!strcmp(a, "WriteEnvironmentStrings") && !in->remove && msidb_table(&in->db, "Environment"))
            logf(in, "Environment table not applied (no persistent environment on NovaOS)");
        return true;
    }
    if (!strcmp(a, "CostFinalize"))      { select_features(in); resolve_directories(in); return true; }
    if (!strcmp(a, "LaunchConditions"))  return action_launch_conditions(in);
    if (!strcmp(a, "AppSearch"))         { action_app_search(in); return true; }
    if (!strcmp(a, "FindRelatedProducts")) { action_find_related(in); return true; }
    if (!strcmp(a, "RemoveExistingProducts")) { action_remove_existing(in); return true; }
    if (!strcmp(a, "CreateFolders"))     { if (!in->remove) action_create_folders(in); return true; }
    if (!strcmp(a, "InstallFiles"))      return in->remove ? true : action_install_files(in);
    if (!strcmp(a, "WriteRegistryValues")) { if (!in->remove) action_write_registry(in); return true; }
    return run_custom_action(in, a);
}

typedef struct { char name[80]; char *condition; int seq; } SeqItem;

static int cmp_seqitem(const void *a, const void *b) { return ((const SeqItem *)a)->seq - ((const SeqItem *)b)->seq; }

static bool run_sequence(Inst *in)
{
    MsiTable *t = msidb_table(&in->db, "InstallExecuteSequence");
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
    } else {
        static const char *std[] = { "LaunchConditions", "FindRelatedProducts", "AppSearch", "CostInitialize",
            "FileCost", "CostFinalize", "InstallValidate", "RemoveExistingProducts", "InstallInitialize",
            "ProcessComponents", "RemoveRegistryValues", "RemoveFiles", "RemoveFolders", "CreateFolders",
            "InstallFiles", "WriteRegistryValues", "CreateShortcuts", "RegisterProduct", "InstallFinalize" };
        n = (int)(sizeof(std) / sizeof(std[0]));
        items = calloc((size_t)n, sizeof(SeqItem));
        for (int i = 0; i < n; i++) { snprintf(items[i].name, sizeof(items[i].name), "%s", std[i]); items[i].condition = strdup(""); }
    }
    bool ok = true;
    bool costed = false;
    for (int i = 0; ok && i < n; i++) {
        if (!strcmp(items[i].name, "CostFinalize")) costed = true;
        if (!cond(in, items[i].condition)) { logf(in, "Action %s skipped (condition)", items[i].name); continue; }
        logf(in, "Action: %s", items[i].name);
        ok = run_action(in, items[i].name);
        if (in->ui && msiui_cancelled(in->ui)) { fail(in, MSI_ERROR_USEREXIT, "Cancelled by the user"); ok = false; }
    }
    if (ok && !costed) { fail(in, MSI_ERROR_PACKAGE_INVALID, "The package has no CostFinalize action"); ok = false; }
    for (int i = 0; i < n; i++) free(items[i].condition);
    free(items);
    return ok;
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
    for (int i = 0; i < in->nprops; i++) { free(in->props[i].name); free(in->props[i].value); }
    free(in->props);
    free(in->feature_on);
    free(in->comp_on);
    msidb_close(&in->db);
    free(in->pkg);
    if (in->log) fclose(in->log);
}

static int run(Inst *in, const MsiRequest *req)
{
    if (req->logfile) {
        in->log = _wfopen(req->logfile, L"wb");
        if (!in->log) logf(in, "Could not open the log file");
    }
    if (!load_package(in, in->pkg_path)) return in->result;
    /* properties: package, system, command line (which wins) */
    system_properties(in);
    parse_cmdline_props(in, req->properties);
    const char *code = get_prop(in, "ProductCode");
    bool installed = product_registered(code, NULL, 0);
    if (installed) set_prop(in, "Installed", "1");
    if (in->remove) {
        if (!installed) logf(in, "Product %s is not registered; removing what the package lists anyway", code);
        else restore_install_location(in, code);
        set_prop(in, "REMOVE", "ALL");
    } else if (installed) {
        set_prop(in, "REINSTALL", "ALL");
        set_prop(in, "REINSTALLMODE", "vomus");
    }
    logf(in, "%s %s %s (%s)", in->remove ? "Removing" : "Installing", get_prop(in, "ProductName"),
         get_prop(in, "ProductVersion"), code);
    if (in->ui) {
        char title[256];
        snprintf(title, sizeof(title), "%s", get_prop(in, "ProductName"));
        msiui_begin(in->ui, title, in->remove);
    }
    run_sequence(in);
    if (!in->result) logf(in, "%s completed successfully", in->remove ? "Removal" : "Installation");
    return in->result;
}

static int run_product(const WCHAR *code, bool remove, int ui_level, MsiUi *ui, char *err, int cap)
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
    Inst *in = calloc(1, sizeof(Inst));
    in->remove = remove;
    in->ui_level = ui_level;
    in->ui = ui;
    wcsncpy(in->pkg_path, pkg, MAX_PATH);
    int r = run(in, &req);
    if (r && err) snprintf(err, (size_t)cap, "%s", in->error);
    inst_free(in);
    free(in);
    return r;
}

int MsiRunInstall(const MsiRequest *req, char *err, int err_cap)
{
    if (err && err_cap) err[0] = 0;
    MsiUi *ui = req->ui_level > MSIUI_NONE ? msiui_create() : NULL;
    int r;
    if (req->product_code && !req->package) {
        r = run_product(req->product_code, req->remove, req->ui_level, ui, err, err_cap);
    } else {
        Inst *in = calloc(1, sizeof(Inst));
        in->remove = req->remove;
        in->ui_level = req->ui_level;
        in->ui = ui;
        wcsncpy(in->pkg_path, req->package ? req->package : L"", MAX_PATH);
        r = run(in, req);
        if (r && err) snprintf(err, (size_t)err_cap, "%s", in->error);
        inst_free(in);
        free(in);
    }
    if (ui) msiui_end(ui, r, req->ui_level >= MSIUI_FULL, err);
    return r;
}

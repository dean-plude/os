/* cabtest.exe — cabinet.dll's File Decompression Interface, as installers
 * use it (WiX Burn extracts the Visual C++ Redistributable's payloads this
 * way): FDIIsCabinet, and FDICopy extracting an MSZIP and an LZX cabinet
 * (cabtest_data.h, made by tools/make_cabtest_data.py) through the caller's
 * file functions, skipping a file, reading a cabinet that sits inside
 * another file (Burn's attached container), following a folder into the
 * next cabinet of a set, and failing cleanly on a missing, foreign or
 * damaged cabinet and when the caller aborts. */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <windows.h>
#include <fdi.h>
#include "cabtest_data.h"

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static char dir[MAX_PATH];                          /* %TEMP%\cabtest\ */
static long base_offset;                            /* where the cabinet starts in the file opened next */

/* ---- the caller's functions (cdecl, as FDI calls them) ---- */
static FNALLOC(mem_alloc) { return HeapAlloc(GetProcessHeap(), 0, cb); }
static FNFREE(mem_free) { HeapFree(GetProcessHeap(), 0, pv); }

typedef struct { HANDLE h; long base; } File;

static FNOPEN(file_open)
{
    (void)pmode;
    DWORD access = (oflag & 3) ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD disp = (oflag & 0x0100) ? CREATE_ALWAYS : OPEN_EXISTING;    /* _O_CREAT */
    HANDLE h = CreateFileA(pszFile, access, FILE_SHARE_READ, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    File *f = HeapAlloc(GetProcessHeap(), 0, sizeof(File));
    f->h = h;
    f->base = base_offset;
    if (f->base) SetFilePointer(h, f->base, NULL, FILE_BEGIN);
    return (INT_PTR)f;
}

static FNREAD(file_read)
{
    DWORD got = 0;
    return ReadFile(((File *)hf)->h, pv, cb, &got, NULL) ? got : (UINT)-1;
}

static FNWRITE(file_write)
{
    DWORD put = 0;
    return WriteFile(((File *)hf)->h, pv, cb, &put, NULL) ? put : (UINT)-1;
}

static FNCLOSE(file_close)
{
    File *f = (File *)hf;
    CloseHandle(f->h);
    HeapFree(GetProcessHeap(), 0, f);
    return 0;
}

static FNSEEK(file_seek)
{
    File *f = (File *)hf;
    if (seektype == SEEK_SET) dist += f->base;
    DWORD r = SetFilePointer(f->h, dist, NULL, seektype);
    return r == INVALID_SET_FILE_POINTER ? -1 : (long)(r - f->base);
}

/* ---- notifications ---- */
typedef struct {
    char outdir[MAX_PATH];
    int  cabinet_info, copy, closed, partial, next_cabinet;
    int  skip;                                      /* index of a file not to extract, or -1 */
    int  abort_at;                                  /* COPY_FILE returns -1 at this file, or -1 */
    bool user_ok;
    char next_seen[64];
    USHORT date, time;
    long sizes[4];
} Run;

static FNFDINOTIFY(notify)
{
    Run *r = pfdin->pv;
    if (r) r->user_ok = r->user_ok && pfdin->pv == r;
    switch (fdint) {
    case fdintCABINET_INFO:
        r->cabinet_info++;
        if (pfdin->psz1 && pfdin->psz1[0] && !r->next_seen[0]) snprintf(r->next_seen, sizeof(r->next_seen), "%s", pfdin->psz1);
        return 0;
    case fdintPARTIAL_FILE:
        r->partial++;
        return 0;
    case fdintNEXT_CABINET:
        r->next_cabinet++;
        return 0;
    case fdintCOPY_FILE: {
        int i = r->copy++;
        if (i < 4) r->sizes[i] = pfdin->cb;
        r->date = pfdin->date;
        r->time = pfdin->time;
        if (i == r->abort_at) return -1;
        if (i == r->skip) return 0;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s%s", r->outdir, pfdin->psz1);
        char *slash = strrchr(path, '\\');
        if (slash > path + strlen(r->outdir)) { *slash = 0; CreateDirectoryA(path, NULL); *slash = '\\'; }
        long keep = base_offset;
        base_offset = 0;
        INT_PTR hf = file_open(path, 0x8000 | 0x0100 | 0x0001 | 0x0200, 0x0180);   /* _O_BINARY|_O_CREAT|_O_WRONLY|_O_TRUNC */
        base_offset = keep;
        return hf;
    }
    case fdintCLOSE_FILE_INFO:
        r->closed++;
        file_close(pfdin->hf);
        return TRUE;
    default:
        return 0;
    }
}

/* ---- helpers ---- */
static unsigned crc32_of(const unsigned char *p, size_t n)
{
    unsigned c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void write_file(const char *name, const void *data, size_t n, size_t prefix)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", dir, name);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put;
    static const char junk[] = "MZ not a cabinet, a stub before the attached container. ";
    for (size_t i = 0; i < prefix; i++) WriteFile(h, &junk[i % (sizeof(junk) - 1)], 1, &put, NULL);
    WriteFile(h, data, (DWORD)n, &put, NULL);
    CloseHandle(h);
}

/* the extracted file matches its size and CRC; -1: missing */
static int matches(const char *outdir, int i)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", outdir, cab_expect[i].name);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    static unsigned char buf[200000];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf), &got, NULL);
    CloseHandle(h);
    return got == cab_expect[i].size && crc32_of(buf, got) == cab_expect[i].crc;
}

static void run_init(Run *r, const char *sub)
{
    memset(r, 0, sizeof(*r));
    r->skip = r->abort_at = -1;
    r->user_ok = true;
    snprintf(r->outdir, sizeof(r->outdir), "%s%s\\", dir, sub);
    CreateDirectoryA(r->outdir, NULL);
}

static void extract(HFDI h, ERF *erf, const char *cab, const char *what)
{
    char name[64], msg[96];
    snprintf(name, sizeof(name), "%s.cab", cab);
    Run r;
    run_init(&r, cab);
    BOOL ok = FDICopy(h, name, dir, 0, notify, NULL, &r);
    snprintf(msg, sizeof(msg), "FDICopy %s", what);
    CHECK(msg, ok && !erf->fError);
    CHECK("one CABINET_INFO", r.cabinet_info == 1);
    CHECK("COPY_FILE and CLOSE_FILE_INFO per file", r.copy == 2 && r.closed == 2);
    CHECK("COPY_FILE sizes", r.sizes[0] == (long)cab_expect[0].size && r.sizes[1] == (long)cab_expect[1].size);
    CHECK("COPY_FILE date and time", r.date == ((45 << 9) | (6 << 5) | 11) && r.time == ((5 << 11) | (18 << 5) | 26));
    CHECK("notifications get pvUser", r.user_ok);
    snprintf(msg, sizeof(msg), "%s: hello.txt", what);
    CHECK(msg, matches(r.outdir, 0) == 1);
    snprintf(msg, sizeof(msg), "%s: data\\big.bin (three blocks, x86 calls)", what);
    CHECK(msg, matches(r.outdir, 1) == 1);
}

int main(void)
{
    GetTempPathA(sizeof(dir), dir);
    strcat(dir, "cabtest\\");
    CreateDirectoryA(dir, NULL);
    write_file("mszip.cab", cab_mszip, sizeof(cab_mszip), 0);
    write_file("lzx.cab", cab_lzx, sizeof(cab_lzx), 0);
    write_file("span1.cab", cab_span1, sizeof(cab_span1), 0);
    write_file("span2.cab", cab_span2, sizeof(cab_span2), 0);
    write_file("attached.exe", cab_mszip, sizeof(cab_mszip), 4321);
    write_file("notcab.cab", "This is not a cabinet at all, just text.", 40, 0);
    unsigned char bad[sizeof(cab_mszip)];
    memcpy(bad, cab_mszip, sizeof(bad));
    unsigned coff = bad[36] | bad[37] << 8;         /* the first data block's "CK" */
    bad[coff + 8] = 'X';
    write_file("damaged.cab", bad, sizeof(bad), 0);

    ERF erf;
    memset(&erf, 0xAA, sizeof(erf));
    HFDI h = FDICreate(mem_alloc, mem_free, file_open, file_read, file_write, file_close, file_seek, cpuUNKNOWN, &erf);
    CHECK("FDICreate", h != NULL && !erf.fError && erf.erfOper == FDIERROR_NONE);
    if (!h) { printf("cabtest: %d passed, %d failed\n", pass, fail + 1); return 1; }

    /* FDIIsCabinet */
    char path[MAX_PATH];
    FDICABINETINFO info;
    snprintf(path, sizeof(path), "%slzx.cab", dir);
    INT_PTR hf = file_open(path, 0x8000, 0);
    memset(&info, 0xCC, sizeof(info));
    CHECK("FDIIsCabinet(lzx.cab)", FDIIsCabinet(h, hf, &info));
    CHECK("cabinet info", info.cbCabinet == (long)sizeof(cab_lzx) && info.cFolders == 1 && info.cFiles == 2 &&
                          info.setID == 0x4E56 && info.iCabinet == 0 && !info.fReserve && !info.hasprev && !info.hasnext);
    file_close(hf);
    snprintf(path, sizeof(path), "%sspan1.cab", dir);
    hf = file_open(path, 0x8000, 0);
    CHECK("FDIIsCabinet(span1.cab) has a next", FDIIsCabinet(h, hf, &info) && info.hasnext && !info.hasprev);
    file_close(hf);
    snprintf(path, sizeof(path), "%snotcab.cab", dir);
    hf = file_open(path, 0x8000, 0);
    CHECK("FDIIsCabinet(text) is FALSE", !FDIIsCabinet(h, hf, &info));
    file_close(hf);

    /* extraction */
    extract(h, &erf, "mszip", "MSZIP");
    extract(h, &erf, "lzx", "LZX");

    /* COPY_FILE returning 0 skips a file; the next one still comes out */
    Run r;
    run_init(&r, "skip");
    r.skip = 0;
    CHECK("FDICopy skipping a file", FDICopy(h, "lzx.cab", dir, 0, notify, NULL, &r));
    CHECK("skipped file not written", matches(r.outdir, 0) == -1 && r.closed == 1);
    CHECK("file after the skipped one", matches(r.outdir, 1) == 1);

    /* a cabinet inside another file: the caller's open and seek add its offset */
    base_offset = 4321;
    run_init(&r, "attached");
    CHECK("FDICopy of an attached container", FDICopy(h, "attached.exe", dir, 0, notify, NULL, &r));
    base_offset = 0;
    CHECK("attached container's files", matches(r.outdir, 0) == 1 && matches(r.outdir, 1) == 1);

    /* a set of two cabinets: the folder goes on into span2.cab */
    run_init(&r, "span");
    CHECK("FDICopy of a cabinet set", FDICopy(h, "span1.cab", dir, 0, notify, NULL, &r) && !erf.fError);
    CHECK("CABINET_INFO names the next cabinet", !strcmp(r.next_seen, "span2.cab"));
    CHECK("NEXT_CABINET asked once, both cabinets read", r.next_cabinet == 1 && r.cabinet_info == 2);
    CHECK("files of the set", matches(r.outdir, 0) == 1 && matches(r.outdir, 1) == 1 && matches(r.outdir, 2) == 1);
    CHECK("the spanning file is extracted once", r.copy == 3 && r.closed == 3 && r.partial == 0);
    run_init(&r, "span2only");
    FDICopy(h, "span2.cab", dir, 0, notify, NULL, &r);   /* (its folder needs span1.cab's first part) */
    CHECK("second cabinet alone: PARTIAL_FILE for the file from span1.cab", r.partial == 1 && r.cabinet_info == 1);

    /* failures */
    run_init(&r, "fail");
    CHECK("FDICopy of a missing cabinet fails", !FDICopy(h, "missing.cab", dir, 0, notify, NULL, &r));
    CHECK("... FDIERROR_CABINET_NOT_FOUND", erf.fError && erf.erfOper == FDIERROR_CABINET_NOT_FOUND);
    CHECK("FDICopy of a text file fails", !FDICopy(h, "notcab.cab", dir, 0, notify, NULL, &r));
    CHECK("... FDIERROR_NOT_A_CABINET", erf.fError && erf.erfOper == FDIERROR_NOT_A_CABINET);
    CHECK("FDICopy of a damaged cabinet fails", !FDICopy(h, "damaged.cab", dir, 0, notify, NULL, &r));
    CHECK("... FDIERROR_CORRUPT_CABINET", erf.fError && erf.erfOper == FDIERROR_CORRUPT_CABINET);
    run_init(&r, "abort");
    r.abort_at = 1;
    CHECK("FDICopy aborted by the caller fails", !FDICopy(h, "mszip.cab", dir, 0, notify, NULL, &r));
    CHECK("... FDIERROR_USER_ABORT", erf.fError && erf.erfOper == FDIERROR_USER_ABORT && r.closed == 1);

    CHECK("FDIDestroy", FDIDestroy(h));
    printf("cabtest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}

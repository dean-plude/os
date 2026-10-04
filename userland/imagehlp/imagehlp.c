/*
 * imagehlp.dll — the certificate table of a PE file (the Security data
 * directory: WIN_CERTIFICATE entries, 8-byte aligned, at a file offset at
 * the end of the file).  Programs that sign or check their own files list,
 * read, add and remove its entries here.  Checking a signature is
 * wintrust's job, not this DLL's.
 */
#include <windows.h>

#define IH __declspec(dllexport)

typedef struct { DWORD dwLength; WORD wRevision, wCertificateType; BYTE bCertificate[1]; } WIN_CERT_;
#define CERT_HEADER 8
#define ERROR_BAD_EXE_FORMAT_ 193
#define OPT_MAGIC32 0x10B
#define OPT_MAGIC64 0x20B
#define DIRECTORY_SECURITY 4
#define CERT_SECTION_TYPE_ANY_ 0xFF

/* Read @n bytes at @off of @f */
static BOOL read_at(HANDLE f, DWORD off, void *buf, DWORD n)
{
    LARGE_INTEGER at;
    at.QuadPart = off;
    DWORD got = 0;
    return SetFilePointerEx(f, at, 0, FILE_BEGIN) && ReadFile(f, buf, n, &got, 0) && got == n;
}

static BOOL write_at(HANDLE f, DWORD off, const void *buf, DWORD n)
{
    LARGE_INTEGER at;
    at.QuadPart = off;
    DWORD put = 0;
    return SetFilePointerEx(f, at, 0, FILE_BEGIN) && WriteFile(f, buf, n, &put, 0) && put == n;
}

/* Where the Security directory entry is (@dir_at) and what it holds */
static BOOL security_dir(HANDLE f, DWORD *dir_at, DWORD *table, DWORD *size)
{
    IMAGE_DOS_HEADER dos;
    DWORD sig;
    IMAGE_FILE_HEADER fh;
    WORD magic;
    if (!read_at(f, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !read_at(f, (DWORD)dos.e_lfanew, &sig, 4) || sig != IMAGE_NT_SIGNATURE ||
        !read_at(f, (DWORD)dos.e_lfanew + 4, &fh, sizeof(fh)) ||
        !read_at(f, (DWORD)dos.e_lfanew + 4 + sizeof(fh), &magic, 2)) {
        SetLastError(ERROR_BAD_EXE_FORMAT_);
        return FALSE;
    }
    DWORD opt = (DWORD)dos.e_lfanew + 4 + sizeof(fh);
    DWORD dirs = magic == OPT_MAGIC64 ? 112 : magic == OPT_MAGIC32 ? 96 : 0;
    if (!dirs) { SetLastError(ERROR_BAD_EXE_FORMAT_); return FALSE; }
    *dir_at = opt + dirs + DIRECTORY_SECURITY * 8;
    IMAGE_DATA_DIRECTORY d;
    if (!read_at(f, *dir_at, &d, 8)) { SetLastError(ERROR_BAD_EXE_FORMAT_); return FALSE; }
    *table = d.VirtualAddress;                  /* a file offset for this one directory */
    *size = d.Size;
    return TRUE;
}

/* The @index-th entry passing @type: its offset and length; FALSE at the end */
static BOOL entry(HANDLE f, DWORD table, DWORD size, WORD type, DWORD index, DWORD *at, DWORD *len, WORD *etype)
{
    DWORD off = 0, k = 0;
    while (off + CERT_HEADER <= size) {
        WIN_CERT_ h;
        if (!read_at(f, table + off, &h, CERT_HEADER) || h.dwLength < CERT_HEADER || off + h.dwLength > size) return FALSE;
        if (type == CERT_SECTION_TYPE_ANY_ || h.wCertificateType == type) {
            if (k++ == index) { *at = table + off; *len = h.dwLength; if (etype) *etype = h.wCertificateType; return TRUE; }
        }
        off += (h.dwLength + 7) & ~7u;
    }
    return FALSE;
}

IH BOOL WINAPI ImageEnumerateCertificates(HANDLE f, WORD type, PDWORD count, PDWORD indices, DWORD nindices)
{
    DWORD dir_at, table, size, at, len;
    if (!count || !security_dir(f, &dir_at, &table, &size)) { if (!count) SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD n = 0;
    WORD t;
    for (DWORD i = 0; entry(f, table, size, CERT_SECTION_TYPE_ANY_, i, &at, &len, &t); i++) {
        if (type != CERT_SECTION_TYPE_ANY_ && t != type) continue;
        if (indices && n < nindices) indices[n] = i;
        n++;
    }
    *count = n;
    return TRUE;
}

IH BOOL WINAPI ImageGetCertificateHeader(HANDLE f, DWORD index, WIN_CERT_ * hdr)
{
    DWORD dir_at, table, size, at, len;
    if (!security_dir(f, &dir_at, &table, &size)) return FALSE;
    if (!entry(f, table, size, CERT_SECTION_TYPE_ANY_, index, &at, &len, 0)) { SetLastError(ERROR_NOT_FOUND); return FALSE; }
    return read_at(f, at, hdr, CERT_HEADER);
}

IH BOOL WINAPI ImageGetCertificateData(HANDLE f, DWORD index, WIN_CERT_ * cert, PDWORD n)
{
    DWORD dir_at, table, size, at, len;
    if (!n || !security_dir(f, &dir_at, &table, &size)) { if (!n) SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!entry(f, table, size, CERT_SECTION_TYPE_ANY_, index, &at, &len, 0)) { SetLastError(ERROR_NOT_FOUND); return FALSE; }
    if (!cert || *n < len) { *n = len; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    *n = len;
    return read_at(f, at, cert, len);
}

/* Add @cert as the last entry: the table stays the file's tail */
IH BOOL WINAPI ImageAddCertificate(HANDLE f, WIN_CERT_ * cert, PDWORD index)
{
    DWORD dir_at, table, size;
    if (!cert || cert->dwLength < CERT_HEADER) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!security_dir(f, &dir_at, &table, &size)) return FALSE;
    LARGE_INTEGER end;
    if (!GetFileSizeEx(f, &end) || end.HighPart) return FALSE;
    if (!table || !size) { table = (end.LowPart + 7) & ~7u; size = 0; }
    else if (table + size != end.LowPart && ((table + size + 7) & ~7u) != end.LowPart) {
        SetLastError(ERROR_BAD_EXE_FORMAT_);             /* something follows the table */
        return FALSE;
    }
    DWORD at = table + ((size + 7) & ~7u), count = 0, len, where;
    while (entry(f, table, size, CERT_SECTION_TYPE_ANY_, count, &where, &len, 0)) count++;
    static const BYTE zero[8];
    if (at > end.LowPart && !write_at(f, end.LowPart, zero, at - end.LowPart)) return FALSE;
    if (!write_at(f, at, cert, cert->dwLength)) return FALSE;
    DWORD pad = ((cert->dwLength + 7) & ~7u) - cert->dwLength;
    if (pad && !write_at(f, at + cert->dwLength, zero, pad)) return FALSE;
    IMAGE_DATA_DIRECTORY d = { table, at - table + cert->dwLength + pad };
    if (!write_at(f, dir_at, &d, 8)) return FALSE;
    if (index) *index = count;
    return TRUE;
}

/* Remove entry @index, moving the later ones down and shortening the file */
IH BOOL WINAPI ImageRemoveCertificate(HANDLE f, DWORD index)
{
    DWORD dir_at, table, size, at, len;
    if (!security_dir(f, &dir_at, &table, &size)) return FALSE;
    if (!entry(f, table, size, CERT_SECTION_TYPE_ANY_, index, &at, &len, 0)) { SetLastError(ERROR_NOT_FOUND); return FALSE; }
    DWORD gone = (len + 7) & ~7u, from = at + gone, rest = table + size > from ? table + size - from : 0;
    BYTE *buf = rest ? HeapAlloc(GetProcessHeap(), 0, rest) : 0;
    if (rest && (!buf || !read_at(f, from, buf, rest) || !write_at(f, at, buf, rest))) {
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        return FALSE;
    }
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    DWORD left = size > gone ? size - gone : 0;
    IMAGE_DATA_DIRECTORY d = { left ? table : 0, left };
    if (!write_at(f, dir_at, &d, 8)) return FALSE;
    LARGE_INTEGER end;
    end.QuadPart = table + left;
    return SetFilePointerEx(f, end, 0, FILE_BEGIN) && SetEndOfFile(f);
}

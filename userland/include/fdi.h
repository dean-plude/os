/*
 * fdi.h — the File Decompression Interface of cabinet.dll: extracting the
 * files of a Microsoft cabinet through the caller's own memory and file
 * functions.  Written for NovaOS from the documented interface; structure
 * layouts match Windows' (natural alignment, at most 4 on 32-bit).
 */
#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DIAMONDAPI
#define DIAMONDAPI __cdecl
#endif
#ifndef CABINETAPI
#define CABINETAPI                    /* cabinet.dll defines it as dllexport */
#endif

#ifndef _WIN64
#pragma pack(push, 4)
#endif

typedef struct {
    int  erfOper;                     /* an FDIERROR */
    int  erfType;                     /* C runtime errno, when there is one */
    BOOL fError;
} ERF, *PERF;

#define CB_MAX_CHUNK          32768U
#define CB_MAX_DISK           0x7FFFFFFFL
#define CB_MAX_FILENAME       256
#define CB_MAX_CABINET_NAME   256
#define CB_MAX_CAB_PATH       256
#define CB_MAX_DISK_NAME      256

#define tcompMASK_TYPE          0x000F
#define tcompTYPE_NONE          0x0000
#define tcompTYPE_MSZIP         0x0001
#define tcompTYPE_QUANTUM       0x0002
#define tcompTYPE_LZX           0x0003
#define tcompBAD                0x000F
#define tcompMASK_LZX_WINDOW    0x1F00
#define tcompLZX_WINDOW_LO      0x0F00
#define tcompLZX_WINDOW_HI      0x1500
#define tcompSHIFT_LZX_WINDOW   8
#define TCOMPfromLZXWindow(w)   ((USHORT)(((w) << tcompSHIFT_LZX_WINDOW) | tcompTYPE_LZX))

typedef enum {
    FDIERROR_NONE,
    FDIERROR_CABINET_NOT_FOUND,
    FDIERROR_NOT_A_CABINET,
    FDIERROR_UNKNOWN_CABINET_VERSION,
    FDIERROR_CORRUPT_CABINET,
    FDIERROR_ALLOC_FAIL,
    FDIERROR_BAD_COMPR_TYPE,
    FDIERROR_MDI_FAIL,
    FDIERROR_TARGET_FILE,
    FDIERROR_RESERVE_MISMATCH,
    FDIERROR_WRONG_CABINET,
    FDIERROR_USER_ABORT,
    FDIERROR_EOF,
} FDIERROR;

#ifndef _A_NAME_IS_UTF
#define _A_NAME_IS_UTF  0x80
#endif
#ifndef _A_EXEC
#define _A_EXEC         0x40
#endif

typedef void *HFDI;

typedef struct {
    long   cbCabinet;
    USHORT cFolders;
    USHORT cFiles;
    USHORT setID;
    USHORT iCabinet;
    BOOL   fReserve;
    BOOL   hasprev;
    BOOL   hasnext;
} FDICABINETINFO, *PFDICABINETINFO;

typedef enum {
    fdidtNEW_CABINET,
    fdidtNEW_FOLDER,
    fdidtDECRYPT,
} FDIDECRYPTTYPE;

typedef struct {
    FDIDECRYPTTYPE fdidt;
    void          *pvUser;
    union {
        struct { void *pHeaderReserve; USHORT cbHeaderReserve; USHORT setID; int iCabinet; } cabinet;
        struct { void *pFolderReserve; USHORT cbFolderReserve; USHORT iFolder; } folder;
        struct { void *pDataReserve; USHORT cbDataReserve; void *pbData; USHORT cbData;
                 BOOL fSplit; USHORT cbPartial; } decrypt;
    };
} FDIDECRYPT, *PFDIDECRYPT;

typedef void   *(DIAMONDAPI *PFNALLOC)(ULONG cb);
typedef void    (DIAMONDAPI *PFNFREE)(void *pv);
typedef INT_PTR (DIAMONDAPI *PFNOPEN)(char *pszFile, int oflag, int pmode);
typedef UINT    (DIAMONDAPI *PFNREAD)(INT_PTR hf, void *pv, UINT cb);
typedef UINT    (DIAMONDAPI *PFNWRITE)(INT_PTR hf, void *pv, UINT cb);
typedef int     (DIAMONDAPI *PFNCLOSE)(INT_PTR hf);
typedef long    (DIAMONDAPI *PFNSEEK)(INT_PTR hf, long dist, int seektype);
typedef int     (DIAMONDAPI *PFNFDIDECRYPT)(PFDIDECRYPT pfdid);

#define FNALLOC(fn) void *DIAMONDAPI fn(ULONG cb)
#define FNFREE(fn)  void DIAMONDAPI fn(void *pv)
#define FNOPEN(fn)  INT_PTR DIAMONDAPI fn(char *pszFile, int oflag, int pmode)
#define FNREAD(fn)  UINT DIAMONDAPI fn(INT_PTR hf, void *pv, UINT cb)
#define FNWRITE(fn) UINT DIAMONDAPI fn(INT_PTR hf, void *pv, UINT cb)
#define FNCLOSE(fn) int DIAMONDAPI fn(INT_PTR hf)
#define FNSEEK(fn)  long DIAMONDAPI fn(INT_PTR hf, long dist, int seektype)
#define FNFDIDECRYPT(fn) int DIAMONDAPI fn(PFDIDECRYPT pfdid)

typedef struct {
    long    cb;
    char   *psz1;
    char   *psz2;
    char   *psz3;
    void   *pv;
    INT_PTR hf;
    USHORT  date;
    USHORT  time;
    USHORT  attribs;
    USHORT  setID;
    USHORT  iCabinet;
    USHORT  iFolder;
    FDIERROR fdie;
} FDINOTIFICATION, *PFDINOTIFICATION;

typedef enum {
    fdintCABINET_INFO,
    fdintPARTIAL_FILE,
    fdintCOPY_FILE,
    fdintCLOSE_FILE_INFO,
    fdintNEXT_CABINET,
    fdintENUMERATE,
} FDINOTIFICATIONTYPE;

typedef INT_PTR (DIAMONDAPI *PFNFDINOTIFY)(FDINOTIFICATIONTYPE fdint, PFDINOTIFICATION pfdin);
#define FNFDINOTIFY(fn) INT_PTR DIAMONDAPI fn(FDINOTIFICATIONTYPE fdint, PFDINOTIFICATION pfdin)

#define cpuUNKNOWN  (-1)
#define cpu80286    0
#define cpu80386    1

CABINETAPI HFDI DIAMONDAPI FDICreate(PFNALLOC pfnalloc, PFNFREE pfnfree, PFNOPEN pfnopen, PFNREAD pfnread,
                          PFNWRITE pfnwrite, PFNCLOSE pfnclose, PFNSEEK pfnseek, int cpuType, PERF perf);
CABINETAPI BOOL DIAMONDAPI FDIIsCabinet(HFDI hfdi, INT_PTR hf, PFDICABINETINFO pfdici);
CABINETAPI BOOL DIAMONDAPI FDICopy(HFDI hfdi, char *pszCabinet, char *pszCabPath, int flags,
                        PFNFDINOTIFY pfnfdin, PFNFDIDECRYPT pfnfdid, void *pvUser);
CABINETAPI BOOL DIAMONDAPI FDIDestroy(HFDI hfdi);
CABINETAPI BOOL DIAMONDAPI FDITruncateCabinet(HFDI hfdi, char *pszCabinetName, USHORT iFolderToDelete);

#ifndef _WIN64
#pragma pack(pop)
#endif

#ifdef __cplusplus
}
#endif

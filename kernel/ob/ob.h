/*
 * ob.h — NT Object Manager
 *
 * The Object Manager is the foundation of the NT executive.  Almost
 * everything a user-mode program interacts with is an "object" — a
 * reference-counted heap allocation with a type, optional name, and
 * optional security descriptor.
 *
 * Key concepts:
 *
 *  OBJECT_HEADER   — Sits immediately before every managed object body.
 *                    Contains reference count, handle count, type pointer,
 *                    and flags.  Users receive a pointer to the BODY (header+1).
 *
 *  OBJECT_TYPE     — Describes a class of objects.  Contains the name,
 *                    size of body, and operation callbacks (Open, Close,
 *                    Delete, QueryName, Parse).
 *
 *  HANDLE_TABLE    — Per-process flat array mapping HANDLE → (object ptr,
 *                    access mask).  HANDLE values are indices × 4 + 4
 *                    (matching NT's table entry granularity; 0 = invalid).
 *
 *  NT Namespace    — A directory tree rooted at "\".  Objects are inserted
 *                    by name and looked up via ObReferenceObjectByName().
 *                    Well-known directories:
 *                      \Device          — kernel device objects
 *                      \BaseNamedObjects — mutexes, events, semaphores
 *                      \Registry        — root of the registry namespace
 *                      \KernelObjects   — internal kernel objects
 *
 * Reference counting rules (same as Windows):
 *   ObCreateObject   → PointerCount = 1
 *   ObInsertObject   → PointerCount++ per handle; HandleCount++
 *   NtClose          → HandleCount--; PointerCount--; if both 0, Delete
 *   ObDereferenceObject → PointerCount--; if 0, Delete
 *
 * Access masks follow the Windows ACCESS_MASK convention:
 *   Bits 0–15:   Object-type-specific rights (e.g. PROCESS_QUERY_INFORMATION)
 *   Bits 16–23:  Standard rights (DELETE, READ_CONTROL, WRITE_DAC, etc.)
 *   Bit 24:      ACCESS_SYSTEM_SECURITY
 *   Bits 25–27:  Reserved
 *   Bits 28–31:  Generic rights (GENERIC_READ, GENERIC_WRITE, etc.)
 */

#pragma once

#include "../include/types.h"

/* -----------------------------------------------------------------------
 * Access mask
 * ----------------------------------------------------------------------- */
typedef ULONG ACCESS_MASK;

#define DELETE                    0x00010000UL
#define READ_CONTROL              0x00020000UL
#define WRITE_DAC                 0x00040000UL
#define WRITE_OWNER               0x00080000UL
#define SYNCHRONIZE               0x00100000UL
#define STANDARD_RIGHTS_REQUIRED  0x000F0000UL
#define STANDARD_RIGHTS_ALL       0x001F0000UL
#define ACCESS_SYSTEM_SECURITY    0x01000000UL
#define MAXIMUM_ALLOWED           0x02000000UL
#define GENERIC_READ              0x80000000UL
#define GENERIC_WRITE             0x40000000UL
#define GENERIC_EXECUTE           0x20000000UL
#define GENERIC_ALL               0x10000000UL

/* Commonly used combinations */
#define PROCESS_ALL_ACCESS        (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0xFFFF)
#define THREAD_ALL_ACCESS         (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0xFFFF)
#define EVENT_ALL_ACCESS          (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x0003)
#define MUTANT_ALL_ACCESS         (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x0001)
#define SEMAPHORE_ALL_ACCESS      (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x0003)
#ifndef KEY_ALL_ACCESS
#define KEY_ALL_ACCESS            (STANDARD_RIGHTS_REQUIRED | 0x003F)
#endif
#define TOKEN_ALL_ACCESS          (STANDARD_RIGHTS_REQUIRED | 0x01FF)
#ifndef FILE_ALL_ACCESS
#define FILE_ALL_ACCESS           (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x01FF)
#endif
#define DIRECTORY_ALL_ACCESS      (STANDARD_RIGHTS_REQUIRED | 0x000F)

/* -----------------------------------------------------------------------
 * UNICODE_STRING — NT's standard string type (length + buffer, no NUL)
 * ----------------------------------------------------------------------- */
typedef struct _UNICODE_STRING {
    USHORT  Length;         /* Length in bytes (NOT including NUL) */
    USHORT  MaximumLength;  /* Buffer capacity in bytes */
    WCHAR  *Buffer;         /* UTF-16LE character data */
} UNICODE_STRING, *PUNICODE_STRING;

/* Initialize a UNICODE_STRING from a compile-time UTF-16 literal.
 * Length = byte_count, MaximumLength = byte_count + 2 (includes NUL) */
#define RTL_CONSTANT_STRING(s) \
    { sizeof(s) - sizeof(WCHAR), sizeof(s), (WCHAR *)(s) }

/* -----------------------------------------------------------------------
 * Object type callbacks
 * ----------------------------------------------------------------------- */
struct _OBJECT_TYPE;
struct _OBJECT_HEADER;

typedef NTSTATUS (*OB_OPEN_METHOD)(
    UINT32 OpenReason, void *Process,
    void *Object, ACCESS_MASK GrantedAccess, UINT32 HandleCount);

typedef void (*OB_CLOSE_METHOD)(
    void *Process, void *Object,
    UINT64 GrantedAccess, UINT64 HandleCount);

typedef void (*OB_DELETE_METHOD)(void *Object);

typedef NTSTATUS (*OB_QUERY_NAME_METHOD)(
    void *Object, bool HasObjectName,
    UNICODE_STRING *ObjectName, UINT32 Length, UINT32 *ReturnLength);

typedef NTSTATUS (*OB_PARSE_METHOD)(
    void *ParseObject, void *ObjectType,
    void *AccessState, UINT8 AccessMode,
    UINT32 Attributes, UNICODE_STRING *CompleteName,
    UNICODE_STRING *RemainingName, void *Context,
    void *SecurityQos, void **Object);

typedef struct _OB_TYPE_OPERATIONS {
    OB_OPEN_METHOD       Open;
    OB_CLOSE_METHOD      Close;
    OB_DELETE_METHOD     Delete;
    OB_QUERY_NAME_METHOD QueryName;
    OB_PARSE_METHOD      Parse;
} OB_TYPE_OPERATIONS;

/* -----------------------------------------------------------------------
 * OBJECT_TYPE — describes a class of objects
 * ----------------------------------------------------------------------- */
#define OBJECT_TYPE_NAME_MAX 32

typedef struct _OBJECT_TYPE {
    /* Linkage in the type list */
    struct _OBJECT_TYPE *next;

    /* Type identity */
    char             Name[OBJECT_TYPE_NAME_MAX];
    UINT32           TypeIndex;       /* Sequential index (used in type table) */

    /* Object body size (not counting the OBJECT_HEADER) */
    UINT32           DefaultBodySize;

    /* Statistics */
    volatile UINT32  TotalNumberOfObjects;
    volatile UINT32  TotalNumberOfHandles;

    /* Callbacks */
    OB_TYPE_OPERATIONS Operations;

    /* Generic mapping (maps GENERIC_* bits to specific rights) */
    ULONG GenericRead;
    ULONG GenericWrite;
    ULONG GenericExecute;
    ULONG GenericAll;
} OBJECT_TYPE, *POBJECT_TYPE;

/* -----------------------------------------------------------------------
 * OBJECT_HEADER — sits immediately before every object body
 * ----------------------------------------------------------------------- */
#define OB_FLAG_PERMANENT        0x10  /* Object survives handle count = 0 */
#define OB_FLAG_NAMED            0x20  /* Object has a name in the namespace */
#define OB_FLAG_KERNEL           0x40  /* Kernel-mode only */

typedef struct _OBJECT_HEADER {
    volatile LONG    PointerCount;  /* Reference count (atomic) */
    volatile LONG    HandleCount;   /* Number of open handles */
    POBJECT_TYPE     Type;
    UINT32           Flags;         /* OB_FLAG_* */
    /* Optional: name index for named objects */
    struct _OBJECT_DIRECTORY_ENTRY *NameEntry;
    /* Body follows immediately */
} OBJECT_HEADER, *POBJECT_HEADER;

/* Convert between body pointer and header */
#define OBJECT_HEADER_TO_BODY(hdr)    ((void *)((OBJECT_HEADER *)(hdr) + 1))
#define BODY_TO_OBJECT_HEADER(body)   ((OBJECT_HEADER *)(body) - 1)
#define OBJECT_TYPE_OF(body)          (BODY_TO_OBJECT_HEADER(body)->Type)

/* -----------------------------------------------------------------------
 * NT Object Namespace — directory tree
 * ----------------------------------------------------------------------- */

#define OBJ_DIRECTORY_HASH_BUCKETS 37  /* Prime for good distribution */
#define OBJ_NAME_MAX               256

typedef struct _OBJECT_DIRECTORY_ENTRY {
    struct _OBJECT_DIRECTORY_ENTRY *HashNext;  /* Next in hash chain */
    void                           *Object;    /* Points to object body */
    UNICODE_STRING                  Name;      /* Object's name in this dir */
    WCHAR                           NameBuf[OBJ_NAME_MAX];
} OBJECT_DIRECTORY_ENTRY, *POBJECT_DIRECTORY_ENTRY;

/* The Directory object type body */
typedef struct _OBJECT_DIRECTORY {
    OBJECT_DIRECTORY_ENTRY *HashBuckets[OBJ_DIRECTORY_HASH_BUCKETS];
    UINT32                  TotalEntries;
} OBJECT_DIRECTORY, *POBJECT_DIRECTORY;

/* -----------------------------------------------------------------------
 * HANDLE_TABLE — per-process handle → object mapping
 * ----------------------------------------------------------------------- */
#define HANDLE_TABLE_INITIAL_SIZE  64
#define HANDLE_TABLE_GROW_BY       64
#define INVALID_HANDLE_VALUE       ((HANDLE)(ULONG_PTR)-1)

/* In NT, HANDLE = (index + 1) << 2. index 0 = handle 4. */
typedef UINT64 HANDLE;

typedef struct _HANDLE_TABLE_ENTRY {
    void        *Object;       /* NULL = free slot */
    ACCESS_MASK  GrantedAccess;
    UINT32       Attributes;   /* OBJ_INHERIT etc. */
} HANDLE_TABLE_ENTRY;

typedef struct _HANDLE_TABLE {
    HANDLE_TABLE_ENTRY  *Entries;
    UINT32               Capacity;
    UINT32               NextFree;   /* Hint for next free slot search */
    volatile UINT32      LockNext, LockOwner;  /* Ticket lock */
} HANDLE_TABLE, *PHANDLE_TABLE;

/* -----------------------------------------------------------------------
 * OBJECT_ATTRIBUTES — passed to Nt* functions to specify an object
 * ----------------------------------------------------------------------- */
#define OBJ_INHERIT             0x00000002UL
#define OBJ_PERMANENT           0x00000010UL
#define OBJ_EXCLUSIVE           0x00000020UL
#define OBJ_CASE_INSENSITIVE    0x00000040UL
#define OBJ_OPENIF              0x00000080UL
#define OBJ_OPENLINK            0x00000100UL
#define OBJ_KERNEL_HANDLE       0x00000200UL
#define OBJ_FORCE_ACCESS_CHECK  0x00000400UL
#define OBJ_VALID_ATTRIBUTES    0x000007F2UL

typedef struct _OBJECT_ATTRIBUTES {
    ULONG            Length;
    HANDLE           RootDirectory;
    PUNICODE_STRING  ObjectName;
    ULONG            Attributes;
    void            *SecurityDescriptor;
    void            *SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

/* -----------------------------------------------------------------------
 * Well-known object type indices
 * ----------------------------------------------------------------------- */
#define OB_TYPE_IDX_DIRECTORY    0
#define OB_TYPE_IDX_SYMLINK      1
#define OB_TYPE_IDX_PROCESS      2
#define OB_TYPE_IDX_THREAD       3
#define OB_TYPE_IDX_EVENT        4
#define OB_TYPE_IDX_MUTANT       5
#define OB_TYPE_IDX_SEMAPHORE    6
#define OB_TYPE_IDX_TIMER        7
#define OB_TYPE_IDX_TOKEN        8
#define OB_TYPE_IDX_FILE         9
#define OB_TYPE_IDX_KEY          10
#define OB_TYPE_IDX_SECTION      11
#define OB_TYPE_IDX_MAX          16

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* Initialize the Object Manager.  Must be called before all Ob* functions. */
void ObInitialize(void);

/*
 * Register a new object type.
 * @type: pre-filled OBJECT_TYPE struct (name, body size, callbacks, generic map)
 * Returns a stable pointer (same as input); assigns TypeIndex.
 */
NTSTATUS ObCreateObjectType(POBJECT_TYPE type);

/*
 * Allocate a new object of the given type.
 * The caller receives a pointer to the body (not the header).
 * PointerCount = 1 on return.  The object is NOT inserted into the
 * namespace — call ObInsertObject() for that.
 */
NTSTATUS ObCreateObject(
    POBJECT_TYPE   Type,
    POBJECT_ATTRIBUTES Attributes,      /* optional — for name/flags */
    UINT32         AdditionalBodySize,  /* extra bytes beyond Type->DefaultBodySize */
    void         **Object);

/*
 * Insert an object into the current process's handle table.
 * Optionally inserts a name into the NT namespace.
 * On success, *Handle receives the new handle value.
 * PointerCount++ and HandleCount++.
 */
NTSTATUS ObInsertObject(
    void        *Object,
    void        *Process,          /* NULL = current process */
    ACCESS_MASK  DesiredAccess,
    UINT32       ObjectPointerBias,
    void       **NewObject,        /* optional: receives final object ptr */
    HANDLE      *Handle);          /* optional: receives handle */

/*
 * Look up an object by handle.
 * Increments PointerCount; caller must call ObDereferenceObject() when done.
 */
NTSTATUS ObReferenceObjectByHandle(
    HANDLE       Handle,
    ACCESS_MASK  DesiredAccess,
    POBJECT_TYPE ExpectedType,     /* NULL = any type */
    void        *Process,          /* NULL = current */
    void       **Object,
    ACCESS_MASK *GrantedAccess);

/*
 * Increment reference count.
 */
void ObReferenceObject(void *Object);

/*
 * Decrement reference count; call Delete callback and free if reaches 0.
 */
void ObDereferenceObject(void *Object);

/*
 * Close a handle (decrements handle count AND pointer count).
 */
NTSTATUS ObCloseHandle(HANDLE Handle, void *Process);

/*
 * Look up an object by absolute NT path (e.g., L"\\Device\\keyboard").
 */
NTSTATUS ObReferenceObjectByName(
    PUNICODE_STRING ObjectName,
    UINT32          Attributes,
    POBJECT_TYPE    ObjectType,
    ACCESS_MASK     DesiredAccess,
    void          **Object);

/* Create a handle table for a new process */
NTSTATUS ObCreateHandleTable(PHANDLE_TABLE *TableOut);

/* Destroy a handle table (close all handles) */
void ObDestroyHandleTable(PHANDLE_TABLE Table);

/* Access the well-known namespace directories */
POBJECT_DIRECTORY ObGetRootDirectory(void);
POBJECT_DIRECTORY ObGetDirectory(const char *path);  /* ASCII path helper */

/* Kernel object types (initialized by ObInitialize) */
extern POBJECT_TYPE ObpDirectoryType;
extern POBJECT_TYPE ObpProcessType;
extern POBJECT_TYPE ObpThreadType;
extern POBJECT_TYPE ObpEventType;
extern POBJECT_TYPE ObpMutantType;
extern POBJECT_TYPE ObpSemaphoreType;
extern POBJECT_TYPE ObpTimerType;
extern POBJECT_TYPE ObpTokenType;
extern POBJECT_TYPE ObpFileType;
extern POBJECT_TYPE ObpKeyType;
extern POBJECT_TYPE ObpSectionType;
extern POBJECT_TYPE ObpSymlinkType;

/*
 * cm.h — NT Configuration Manager (Registry)
 *
 * The Configuration Manager implements the NT registry: a hierarchical
 * persistent key/value store.  In Phase 2 we implement an in-memory
 * registry with the standard NT hive structure:
 *
 *   \Registry\Machine   (HKLM)
 *   \Registry\User      (HKU)
 *   \Registry\User\Default  (HKCU placeholder)
 *
 * Key NT registry value types supported:
 *   REG_NONE, REG_SZ, REG_EXPAND_SZ, REG_BINARY,
 *   REG_DWORD, REG_DWORD_BIG_ENDIAN, REG_LINK,
 *   REG_MULTI_SZ, REG_QWORD
 *
 * Handle encoding: same as ObHandle — KEY objects go through the
 * Object Manager (ObpKeyType).  NtOpenKey/NtCreateKey return HANDLE
 * values that reference CM_KEY_BODY objects.
 *
 * Thread safety: a single global registry spinlock protects all
 * operations (ticket lock, same pattern as PS/OB).  Phase 3 will
 * switch to per-hive locks.
 */

#pragma once

#include "../include/types.h"
#include "../ob/ob.h"

/* -----------------------------------------------------------------------
 * Registry value types (REG_*)
 * ----------------------------------------------------------------------- */
#define REG_NONE                    0
#define REG_SZ                      1   /* NUL-terminated UTF-16LE string */
#define REG_EXPAND_SZ               2   /* Expandable string (contains %env%) */
#define REG_BINARY                  3   /* Raw binary data */
#define REG_DWORD                   4   /* 32-bit little-endian integer */
#define REG_DWORD_LITTLE_ENDIAN     4
#define REG_DWORD_BIG_ENDIAN        5
#define REG_LINK                    6   /* Symbolic link (UNICODE_STRING) */
#define REG_MULTI_SZ                7   /* Sequence of NUL-terminated strings */
#define REG_RESOURCE_LIST           8
#define REG_FULL_RESOURCE_DESCRIPTOR 9
#define REG_RESOURCE_REQUIREMENTS_LIST 10
#define REG_QWORD                   11  /* 64-bit integer */
#define REG_QWORD_LITTLE_ENDIAN     11

/* -----------------------------------------------------------------------
 * KEY_VALUE_BASIC_INFORMATION — returned by NtEnumerateValueKey
 * ----------------------------------------------------------------------- */
typedef struct _KEY_VALUE_BASIC_INFORMATION {
    UINT32  TitleIndex;
    UINT32  Type;
    UINT32  NameLength;
    WCHAR   Name[1];
} KEY_VALUE_BASIC_INFORMATION;

/* -----------------------------------------------------------------------
 * KEY_VALUE_PARTIAL_INFORMATION — returned by NtQueryValueKey
 * ----------------------------------------------------------------------- */
typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
    UINT32  TitleIndex;
    UINT32  Type;
    UINT32  DataLength;
    UINT8   Data[1];
} KEY_VALUE_PARTIAL_INFORMATION;

/* -----------------------------------------------------------------------
 * KEY_VALUE_FULL_INFORMATION — full value data + name
 * ----------------------------------------------------------------------- */
typedef struct _KEY_VALUE_FULL_INFORMATION {
    UINT32  TitleIndex;
    UINT32  Type;
    UINT32  DataOffset;
    UINT32  DataLength;
    UINT32  NameLength;
    WCHAR   Name[1];
} KEY_VALUE_FULL_INFORMATION;

/* -----------------------------------------------------------------------
 * KEY_BASIC_INFORMATION — returned by NtEnumerateKey
 * ----------------------------------------------------------------------- */
typedef struct _KEY_BASIC_INFORMATION {
    UINT64  LastWriteTime;
    UINT32  TitleIndex;
    UINT32  NameLength;
    WCHAR   Name[1];
} KEY_BASIC_INFORMATION;

typedef enum _KEY_VALUE_INFORMATION_CLASS {
    KeyValueBasicInformation     = 0,
    KeyValueFullInformation      = 1,
    KeyValuePartialInformation   = 2,
} KEY_VALUE_INFORMATION_CLASS;

typedef enum _KEY_INFORMATION_CLASS {
    KeyBasicInformation          = 0,
    KeyNodeInformation           = 1,
    KeyFullInformation           = 2,
} KEY_INFORMATION_CLASS;

/* -----------------------------------------------------------------------
 * Internal CM node types (not exposed to callers)
 * ----------------------------------------------------------------------- */

#define CM_VALUE_DATA_MAX  512   /* max inline value data bytes */
#define CM_NAME_MAX        256   /* max key/value name chars (WCHAR) */

typedef struct _CM_VALUE {
    struct _CM_VALUE  *next;
    WCHAR              Name[CM_NAME_MAX];   /* value name (NUL-terminated) */
    UINT32             NameLen;             /* chars (not bytes) */
    UINT32             Type;                /* REG_* */
    UINT32             DataLen;             /* bytes */
    UINT8              Data[CM_VALUE_DATA_MAX];
} CM_VALUE, *PCM_VALUE;

typedef struct _CM_KEY_NODE {
    struct _CM_KEY_NODE  *parent;
    struct _CM_KEY_NODE  *child_head;   /* First subkey (singly-linked) */
    struct _CM_KEY_NODE  *sibling;      /* Next sibling */
    CM_VALUE             *values;       /* Linked list of values */
    UINT32                subkey_count;
    UINT32                value_count;
    WCHAR                 Name[CM_NAME_MAX];
    UINT32                NameLen;
} CM_KEY_NODE, *PCM_KEY_NODE;

/* -----------------------------------------------------------------------
 * CM_KEY_BODY — the object body for Key objects (returned by NtOpenKey)
 * ----------------------------------------------------------------------- */
typedef struct _CM_KEY_BODY {
    CM_KEY_NODE  *Node;      /* The actual registry node */
    UINT32        Flags;
} CM_KEY_BODY, *PCM_KEY_BODY;

/* -----------------------------------------------------------------------
 * Public API (matches NT native API)
 * ----------------------------------------------------------------------- */

/* Initialize the Configuration Manager and create the default hive tree */
void CmInitialize(void);

/*
 * Open an existing registry key.
 * @KeyHandle:       Receives the handle.
 * @DesiredAccess:   KEY_READ, KEY_WRITE, KEY_ALL_ACCESS, etc.
 * @ObjectAttributes: Must have ObjectName pointing to an absolute path
 *                    like L"\\Registry\\Machine\\SYSTEM".
 */
NTSTATUS NtOpenKey(
    HANDLE             *KeyHandle,
    ACCESS_MASK         DesiredAccess,
    POBJECT_ATTRIBUTES  ObjectAttributes);

/*
 * Create or open a registry key.
 * @Disposition: Optional — receives REG_CREATED_NEW_KEY or REG_OPENED_EXISTING_KEY
 */
#define REG_CREATED_NEW_KEY     0x00000001
#define REG_OPENED_EXISTING_KEY 0x00000002

NTSTATUS NtCreateKey(
    HANDLE             *KeyHandle,
    ACCESS_MASK         DesiredAccess,
    POBJECT_ATTRIBUTES  ObjectAttributes,
    UINT32              TitleIndex,
    UNICODE_STRING     *Class,
    UINT32              CreateOptions,
    UINT32             *Disposition);

/* Close a key handle (use NtClose / ObCloseHandle) */

/*
 * Query a named value in a key.
 */
NTSTATUS NtQueryValueKey(
    HANDLE                      KeyHandle,
    UNICODE_STRING             *ValueName,
    KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass,
    void                       *KeyValueInformation,
    UINT32                      Length,
    UINT32                     *ResultLength);

/*
 * Set (create or update) a named value in a key.
 */
NTSTATUS NtSetValueKey(
    HANDLE          KeyHandle,
    UNICODE_STRING *ValueName,
    UINT32          TitleIndex,
    UINT32          Type,
    void           *Data,
    UINT32          DataSize);

/*
 * Delete a named value from a key.
 */
NTSTATUS NtDeleteValueKey(
    HANDLE          KeyHandle,
    UNICODE_STRING *ValueName);

/*
 * Delete a key (key must have no subkeys).
 */
NTSTATUS NtDeleteKey(HANDLE KeyHandle);

/*
 * Enumerate subkeys of a key.
 * @Index: 0-based index.
 */
NTSTATUS NtEnumerateKey(
    HANDLE                 KeyHandle,
    UINT32                 Index,
    KEY_INFORMATION_CLASS  KeyInformationClass,
    void                  *KeyInformation,
    UINT32                 Length,
    UINT32                *ResultLength);

/*
 * Enumerate values of a key.
 * @Index: 0-based index.
 */
NTSTATUS NtEnumerateValueKey(
    HANDLE                      KeyHandle,
    UINT32                      Index,
    KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass,
    void                       *KeyValueInformation,
    UINT32                      Length,
    UINT32                     *ResultLength);

/* -----------------------------------------------------------------------
 * Well-known registry path constants
 * ----------------------------------------------------------------------- */
#define CM_REGISTRY_ROOT        L"\\Registry"
#define CM_HKLM_PATH            L"\\Registry\\Machine"
#define CM_HKU_PATH             L"\\Registry\\User"
#define CM_HKCU_PATH            L"\\Registry\\User\\Default"

/* Access masks for registry keys */
#define KEY_QUERY_VALUE         0x0001
#define KEY_SET_VALUE           0x0002
#define KEY_CREATE_SUB_KEY      0x0004
#define KEY_ENUMERATE_SUB_KEYS  0x0008
#define KEY_NOTIFY              0x0010
#define KEY_CREATE_LINK         0x0020
#define KEY_READ   (STANDARD_RIGHTS_REQUIRED | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY)
#define KEY_WRITE  (STANDARD_RIGHTS_REQUIRED | KEY_SET_VALUE | KEY_CREATE_SUB_KEY)
#define KEY_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | KEY_READ | KEY_WRITE | KEY_CREATE_LINK)

/*
 * se.h — NT Security Reference Monitor (SeXxx)
 *
 * The Security Reference Monitor (SRM) enforces access control for all
 * object manager operations.  Every process has a primary Token (ETOKEN)
 * and every thread can impersonate with an impersonation Token.
 *
 * Key structures (NT-compatible layout):
 *
 *   SID (Security Identifier) — opaque variable-length identifier.
 *     Prefix: S-Revision-IdentifierAuthority-SubAuthority...
 *     e.g.  S-1-5-18    = NT AUTHORITY\SYSTEM
 *           S-1-5-32-544 = BUILTIN\Administrators
 *           S-1-1-0      = Everyone
 *
 *   ACE (Access Control Entry) — one entry in an ACL.
 *     Types: ACCESS_ALLOWED_ACE, ACCESS_DENIED_ACE, SYSTEM_AUDIT_ACE
 *
 *   ACL (Access Control List) — ordered list of ACEs.
 *     DACL: discretionary ACL — who can access the object.
 *     SACL: system ACL — which accesses generate audit events.
 *
 *   SECURITY_DESCRIPTOR — contains owner SID, group SID, DACL, SACL.
 *
 *   TOKEN — represents a security context (user identity + privileges).
 *     Contains: user SID, group list, privilege list, default DACL.
 *
 * Phase 2 scope:
 *   - Well-known SIDs (SYSTEM, Administrators, Everyone)
 *   - Token creation/duplication
 *   - Basic access check (SeAccessCheck)
 *   - No full ACL evaluation (everything passes if kernel-mode)
 *   - Privilege constants from winnt.h
 *
 * NT security model note:
 *   Real NT performs access checks at handle open time (NtOpenProcess etc.)
 *   and stores the granted access mask in the handle table entry.
 *   We do the same — the handle's GrantedAccess is set in ObInsertObject.
 */

#pragma once

#include "../include/types.h"
#include "../ob/ob.h"

/* -----------------------------------------------------------------------
 * SID (Security Identifier)
 * ----------------------------------------------------------------------- */
#define SID_REVISION            1
#define SID_MAX_SUB_AUTHORITIES 15

typedef struct _SID_IDENTIFIER_AUTHORITY {
    UINT8 Value[6];
} SID_IDENTIFIER_AUTHORITY;

typedef struct _SID {
    UINT8                    Revision;
    UINT8                    SubAuthorityCount;
    SID_IDENTIFIER_AUTHORITY IdentifierAuthority;
    UINT32                   SubAuthority[SID_MAX_SUB_AUTHORITIES];
} SID, *PSID;

/* Predefined identifier authorities */
#define SECURITY_NULL_SID_AUTHORITY      {0,0,0,0,0,0}
#define SECURITY_WORLD_SID_AUTHORITY     {0,0,0,0,0,1}
#define SECURITY_LOCAL_SID_AUTHORITY     {0,0,0,0,0,2}
#define SECURITY_CREATOR_SID_AUTHORITY   {0,0,0,0,0,3}
#define SECURITY_NT_AUTHORITY            {0,0,0,0,0,5}

/* Well-known sub-authorities */
#define SECURITY_WORLD_RID              0x00000000L
#define SECURITY_LOCAL_RID              0x00000000L
#define SECURITY_CREATOR_OWNER_RID      0x00000000L
#define SECURITY_CREATOR_GROUP_RID      0x00000001L
#define SECURITY_DIALUP_RID             0x00000001L
#define SECURITY_NETWORK_RID            0x00000002L
#define SECURITY_BATCH_RID              0x00000003L
#define SECURITY_INTERACTIVE_RID        0x00000004L
#define SECURITY_SERVICE_RID            0x00000006L
#define SECURITY_ANONYMOUS_LOGON_RID    0x00000007L
#define SECURITY_LOCAL_SERVICE_RID      0x00000013L
#define SECURITY_NETWORK_SERVICE_RID    0x00000014L
#define SECURITY_LOCAL_SYSTEM_RID       0x00000012L
#define SECURITY_BUILTIN_DOMAIN_RID     0x00000020L
#define DOMAIN_ALIAS_RID_ADMINS         0x00000220L

/* -----------------------------------------------------------------------
 * ACE and ACL
 * ----------------------------------------------------------------------- */
#define ACCESS_MIN_MS_ACE_TYPE          0x0
#define ACCESS_ALLOWED_ACE_TYPE         0x0
#define ACCESS_DENIED_ACE_TYPE          0x1
#define SYSTEM_AUDIT_ACE_TYPE           0x2

#define OBJECT_INHERIT_ACE              0x01
#define CONTAINER_INHERIT_ACE           0x02
#define NO_PROPAGATE_INHERIT_ACE        0x04
#define INHERIT_ONLY_ACE                0x08
#define INHERITED_ACE                   0x10

typedef struct _ACE_HEADER {
    UINT8  AceType;
    UINT8  AceFlags;
    UINT16 AceSize;
} ACE_HEADER;

typedef struct _ACCESS_ALLOWED_ACE {
    ACE_HEADER  Header;
    ACCESS_MASK Mask;
    UINT32      SidStart;   /* First DWORD of SID (variable length follows) */
} ACCESS_ALLOWED_ACE;

typedef struct _ACCESS_DENIED_ACE {
    ACE_HEADER  Header;
    ACCESS_MASK Mask;
    UINT32      SidStart;
} ACCESS_DENIED_ACE;

#define ACL_REVISION    2

typedef struct _ACL {
    UINT8  AclRevision;
    UINT8  Sbz1;
    UINT16 AclSize;
    UINT16 AceCount;
    UINT16 Sbz2;
    /* ACEs follow */
} ACL, *PACL;

/* -----------------------------------------------------------------------
 * SECURITY_DESCRIPTOR
 * ----------------------------------------------------------------------- */
#define SECURITY_DESCRIPTOR_REVISION    1

/* Control flags */
#define SE_OWNER_DEFAULTED      0x0001
#define SE_GROUP_DEFAULTED      0x0002
#define SE_DACL_PRESENT         0x0004
#define SE_DACL_DEFAULTED       0x0008
#define SE_SACL_PRESENT         0x0010
#define SE_SACL_DEFAULTED       0x0020
#define SE_SELF_RELATIVE        0x8000

typedef struct _SECURITY_DESCRIPTOR {
    UINT8     Revision;
    UINT8     Sbz1;
    UINT16    Control;   /* SE_* flags */
    PSID      Owner;
    PSID      Group;
    PACL      Sacl;
    PACL      Dacl;
} SECURITY_DESCRIPTOR, *PSECURITY_DESCRIPTOR;

/* -----------------------------------------------------------------------
 * Privilege values (SE_* LUID constants from winnt.h)
 * ----------------------------------------------------------------------- */
#define SE_MIN_WELL_KNOWN_PRIVILEGE         2L
#define SE_CREATE_TOKEN_PRIVILEGE           2L
#define SE_ASSIGNPRIMARYTOKEN_PRIVILEGE     3L
#define SE_LOCK_MEMORY_PRIVILEGE            4L
#define SE_INCREASE_QUOTA_PRIVILEGE         5L
#define SE_MACHINE_ACCOUNT_PRIVILEGE        6L
#define SE_TCB_PRIVILEGE                    7L
#define SE_SECURITY_PRIVILEGE               8L
#define SE_TAKE_OWNERSHIP_PRIVILEGE         9L
#define SE_LOAD_DRIVER_PRIVILEGE            10L
#define SE_SYSTEM_PROFILE_PRIVILEGE         11L
#define SE_SYSTEMTIME_PRIVILEGE             12L
#define SE_PROF_SINGLE_PROCESS_PRIVILEGE    13L
#define SE_INC_BASE_PRIORITY_PRIVILEGE      14L
#define SE_CREATE_PAGEFILE_PRIVILEGE        15L
#define SE_CREATE_PERMANENT_PRIVILEGE       16L
#define SE_BACKUP_PRIVILEGE                 17L
#define SE_RESTORE_PRIVILEGE                18L
#define SE_SHUTDOWN_PRIVILEGE               19L
#define SE_DEBUG_PRIVILEGE                  20L
#define SE_AUDIT_PRIVILEGE                  21L
#define SE_SYSTEM_ENVIRONMENT_PRIVILEGE     22L
#define SE_CHANGE_NOTIFY_PRIVILEGE          23L
#define SE_REMOTE_SHUTDOWN_PRIVILEGE        24L
#define SE_UNDOCK_PRIVILEGE                 25L
#define SE_SYNC_AGENT_PRIVILEGE             26L
#define SE_ENABLE_DELEGATION_PRIVILEGE      27L
#define SE_MANAGE_VOLUME_PRIVILEGE          28L
#define SE_IMPERSONATE_PRIVILEGE            29L
#define SE_CREATE_GLOBAL_PRIVILEGE          30L
#define SE_TRUSTED_CREDMAN_ACCESS_PRIVILEGE 31L
#define SE_RELABEL_PRIVILEGE                32L
#define SE_INC_WORKING_SET_PRIVILEGE        33L
#define SE_TIME_ZONE_PRIVILEGE              34L
#define SE_CREATE_SYMBOLIC_LINK_PRIVILEGE   35L
#define SE_MAX_WELL_KNOWN_PRIVILEGE         SE_CREATE_SYMBOLIC_LINK_PRIVILEGE

/* Privilege attribute flags */
#define SE_PRIVILEGE_ENABLED_BY_DEFAULT     0x00000001L
#define SE_PRIVILEGE_ENABLED                0x00000002L
#define SE_PRIVILEGE_REMOVED                0x00000004L
#define SE_PRIVILEGE_USED_FOR_ACCESS        0x80000000L

/* -----------------------------------------------------------------------
 * LUID (Locally Unique Identifier)
 * ----------------------------------------------------------------------- */
typedef struct _LUID {
    UINT32 LowPart;
    INT32  HighPart;
} LUID;

#define LUID_FROM_PRIVILEGE(p) ((LUID){ .LowPart = (UINT32)(p), .HighPart = 0 })

typedef struct _LUID_AND_ATTRIBUTES {
    LUID   Luid;
    UINT32 Attributes;
} LUID_AND_ATTRIBUTES;

/* -----------------------------------------------------------------------
 * TOKEN — security context
 * ----------------------------------------------------------------------- */
#define TOKEN_PRIVILEGES_MAX   35   /* SE_MAX_WELL_KNOWN_PRIVILEGE + 1 */
#define TOKEN_GROUPS_MAX       16

#define TOKEN_TYPE_PRIMARY       1
#define TOKEN_TYPE_IMPERSONATION 2

typedef struct _SID_AND_ATTRIBUTES {
    PSID   Sid;
    UINT32 Attributes;
} SID_AND_ATTRIBUTES;

/* Group attribute flags */
#define SE_GROUP_MANDATORY          0x00000001L
#define SE_GROUP_ENABLED_BY_DEFAULT 0x00000002L
#define SE_GROUP_ENABLED            0x00000004L
#define SE_GROUP_OWNER              0x00000008L
#define SE_GROUP_USE_FOR_DENY_ONLY  0x00000010L
#define SE_GROUP_INTEGRITY          0x00000020L
#define SE_GROUP_INTEGRITY_ENABLED  0x00000040L
#define SE_GROUP_LOGON_ID           0xC0000000L
#define SE_GROUP_RESOURCE           0x20000000L

typedef struct _TOKEN {
    /* Token identity */
    LUID             TokenId;
    LUID             AuthenticationId;
    UINT32           TokenType;   /* TOKEN_TYPE_* */
    UINT32           ImpersonationLevel;

    /* User (index 0 of UserAndGroups) */
    SID_AND_ATTRIBUTES UserAndGroups[TOKEN_GROUPS_MAX + 1];
    UINT32             UserAndGroupCount;

    /* Privileges */
    LUID_AND_ATTRIBUTES Privileges[TOKEN_PRIVILEGES_MAX];
    UINT32              PrivilegeCount;

    /* Default DACL */
    PACL     DefaultDacl;

    /* Inline SID storage for the token's SIDs */
    /* We embed up to 4 SIDs inline (USER + SYSTEM + Admins + Everyone) */
    SID      InlineSids[4];
    UINT32   InlineSidCount;
} TOKEN, *PTOKEN;

/* -----------------------------------------------------------------------
 * Well-known SID pointers (initialized by SeInitialize)
 * ----------------------------------------------------------------------- */
extern PSID SeWorldSid;       /* S-1-1-0  Everyone */
extern PSID SeLocalSystemSid; /* S-1-5-18 SYSTEM */
extern PSID SeAdminsSid;      /* S-1-5-32-544 Administrators */
extern PSID SeLocalServiceSid;    /* S-1-5-19 */
extern PSID SeNetworkServiceSid;  /* S-1-5-20 */

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* Initialize the security subsystem and create well-known SIDs */
void SeInitialize(void);

/*
 * Create the system token (assigned to PsInitialSystemProcess).
 * The system token has all privileges enabled and runs as SYSTEM.
 */
NTSTATUS SeCreateSystemToken(PTOKEN *TokenOut);

/*
 * Duplicate a token (used by NtDuplicateToken).
 */
NTSTATUS SeDuplicateToken(PTOKEN SourceToken, UINT32 TokenType,
                           PTOKEN *NewToken);

/*
 * Access check: returns TRUE if the given token has DesiredAccess
 * on an object with SecurityDescriptor.  Phase 2 always grants
 * access for kernel-mode callers.
 */
bool SeAccessCheck(
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    PTOKEN               Token,
    ACCESS_MASK          DesiredAccess,
    ACCESS_MASK         *GrantedAccess);

/*
 * Test if a token has a specific privilege enabled.
 */
bool SePrivilegeCheck(PTOKEN Token, UINT32 PrivilegeValue);

/*
 * Free a token (decrements reference count).
 */
void SeDereferenceToken(PTOKEN Token);

/*
 * Get the effective token for the current thread
 * (impersonation token if set, otherwise process primary token).
 */
PTOKEN SeGetCurrentToken(void);

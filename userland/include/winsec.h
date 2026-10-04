/* winsec.h — security and crypto types and the advapi32 functions that use
 * them (SIDs, tokens, security descriptors, CryptoAPI, event tracing,
 * services, credentials).  Included by windows.h. */
#pragma once

typedef DWORD ACCESS_MASK, *PACCESS_MASK;
typedef BOOL *PBOOL;
typedef UCHAR *PUCHAR;
typedef struct _LUID { DWORD LowPart; LONG HighPart; } LUID, *PLUID;
typedef struct _LUID_AND_ATTRIBUTES { LUID Luid; DWORD Attributes; } LUID_AND_ATTRIBUTES;
typedef struct _SID_IDENTIFIER_AUTHORITY { BYTE Value[6]; } SID_IDENTIFIER_AUTHORITY, *PSID_IDENTIFIER_AUTHORITY;
typedef struct _SID {
    BYTE Revision, SubAuthorityCount;
    SID_IDENTIFIER_AUTHORITY IdentifierAuthority;
    DWORD SubAuthority[1];
} SID;
typedef PVOID PSID;
typedef struct _SID_AND_ATTRIBUTES { PSID Sid; DWORD Attributes; } SID_AND_ATTRIBUTES, *PSID_AND_ATTRIBUTES;
typedef struct _ACL { BYTE AclRevision, Sbz1; WORD AclSize, AceCount, Sbz2; } ACL, *PACL;
typedef struct _ACE_HEADER { BYTE AceType, AceFlags; WORD AceSize; } ACE_HEADER;
typedef struct _ACCESS_ALLOWED_ACE { ACE_HEADER Header; ACCESS_MASK Mask; DWORD SidStart; } ACCESS_ALLOWED_ACE;
typedef WORD SECURITY_DESCRIPTOR_CONTROL, *PSECURITY_DESCRIPTOR_CONTROL;
typedef struct _SECURITY_DESCRIPTOR {
    BYTE Revision, Sbz1;
    SECURITY_DESCRIPTOR_CONTROL Control;
    PSID Owner, Group;
    PACL Sacl, Dacl;
} SECURITY_DESCRIPTOR;
typedef PVOID PSECURITY_DESCRIPTOR;
typedef DWORD SECURITY_INFORMATION;
typedef struct _GENERIC_MAPPING { ACCESS_MASK GenericRead, GenericWrite, GenericExecute, GenericAll; } GENERIC_MAPPING, *PGENERIC_MAPPING;
typedef struct _PRIVILEGE_SET { DWORD PrivilegeCount, Control; LUID_AND_ATTRIBUTES Privilege[1]; } PRIVILEGE_SET, *PPRIVILEGE_SET;
typedef struct _TOKEN_PRIVILEGES { DWORD PrivilegeCount; LUID_AND_ATTRIBUTES Privileges[1]; } TOKEN_PRIVILEGES, *PTOKEN_PRIVILEGES;
typedef struct _TOKEN_USER { SID_AND_ATTRIBUTES User; } TOKEN_USER;
typedef LUID_AND_ATTRIBUTES *PLUID_AND_ATTRIBUTES;
typedef struct _TOKEN_GROUPS { DWORD GroupCount; SID_AND_ATTRIBUTES Groups[1]; } TOKEN_GROUPS, *PTOKEN_GROUPS;
typedef enum { TokenPrimary = 1, TokenImpersonation } TOKEN_TYPE;
typedef struct _TOKEN_ELEVATION { DWORD TokenIsElevated; } TOKEN_ELEVATION;
typedef enum { TokenUser = 1, TokenGroups, TokenPrivileges, TokenOwner, TokenPrimaryGroup, TokenDefaultDacl, TokenSource,
               TokenType, TokenImpersonationLevel, TokenStatistics, TokenRestrictedSids, TokenSessionId,
               TokenGroupsAndPrivileges, TokenSessionReference, TokenSandBoxInert, TokenAuditPolicy, TokenOrigin,
               TokenElevationType, TokenLinkedToken, TokenElevation, TokenHasRestrictions, TokenAccessInformation,
               TokenVirtualizationAllowed, TokenVirtualizationEnabled, TokenIntegrityLevel, TokenUIAccess,
               TokenMandatoryPolicy, TokenLogonSid, TokenIsAppContainer } TOKEN_INFORMATION_CLASS;
typedef enum { SecurityAnonymous, SecurityIdentification, SecurityImpersonation, SecurityDelegation } SECURITY_IMPERSONATION_LEVEL;
typedef struct _SECURITY_QUALITY_OF_SERVICE {
    DWORD Length;
    SECURITY_IMPERSONATION_LEVEL ImpersonationLevel;
    BYTE ContextTrackingMode;
    BOOLEAN EffectiveOnly;
} SECURITY_QUALITY_OF_SERVICE;
typedef enum { SidTypeUser = 1, SidTypeGroup, SidTypeDomain, SidTypeAlias, SidTypeWellKnownGroup, SidTypeDeletedAccount,
               SidTypeInvalid, SidTypeUnknown, SidTypeComputer, SidTypeLabel } SID_NAME_USE, *PSID_NAME_USE;
typedef enum { SE_UNKNOWN_OBJECT_TYPE, SE_FILE_OBJECT, SE_SERVICE, SE_PRINTER, SE_REGISTRY_KEY,
               SE_LMSHARE, SE_KERNEL_OBJECT, SE_WINDOW_OBJECT } SE_OBJECT_TYPE;

#define TOKEN_QUERY              0x0008
#define TOKEN_ADJUST_PRIVILEGES  0x0020
#define TOKEN_DUPLICATE          0x0002
#define TOKEN_IMPERSONATE        0x0004
#define TOKEN_ALL_ACCESS         0xF01FF
#define TOKEN_ASSIGN_PRIMARY     0x0001
#define TOKEN_ADJUST_DEFAULT     0x0080
#define DISABLE_MAX_PRIVILEGE    0x1         /* CreateRestrictedToken flags */
#define SANDBOX_INERT            0x2
#define WRITE_RESTRICTED         0x8
#define SE_GROUP_ENABLED         0x00000004
#define SE_GROUP_USE_FOR_DENY_ONLY 0x00000010
#define SE_PRIVILEGE_ENABLED     0x00000002
#define OWNER_SECURITY_INFORMATION 0x00000001
#define GROUP_SECURITY_INFORMATION 0x00000002
#define DACL_SECURITY_INFORMATION  0x00000004
#define SECURITY_DESCRIPTOR_REVISION 1
#define ACL_REVISION             2
#define SE_DACL_PRESENT          0x0004
#define SE_SELF_RELATIVE         0x8000
#define SECURITY_NT_AUTHORITY    { 0, 0, 0, 0, 0, 5 }
#define SECURITY_WORLD_SID_AUTHORITY { 0, 0, 0, 0, 0, 1 }
#define SECURITY_BUILTIN_DOMAIN_RID 0x20
#define DOMAIN_ALIAS_RID_ADMINS  0x220
#define DOMAIN_ALIAS_RID_USERS   0x221
#define SECURITY_MAX_SID_SIZE    68

typedef ULONG_PTR HCRYPTPROV, HCRYPTHASH, HCRYPTKEY;
#define PROV_RSA_FULL            1
#define PROV_RSA_AES             24
#define CRYPT_VERIFYCONTEXT      0xF0000000
#define CALG_MD5                 0x8003
#define CALG_SHA1                0x8004
#define CALG_SHA_256             0x800c
#define CALG_SHA_384             0x800d
#define CALG_SHA_512             0x800e
#define HP_HASHVAL               0x0002
#define HP_HASHSIZE              0x0004

/* SIDs */
WINADVAPI BOOL  WINAPI AllocateAndInitializeSid(PSID_IDENTIFIER_AUTHORITY a, BYTE n, DWORD s0, DWORD s1, DWORD s2, DWORD s3,
                                                DWORD s4, DWORD s5, DWORD s6, DWORD s7, PSID *sid);
WINADVAPI PVOID WINAPI FreeSid(PSID sid);
WINADVAPI BOOL  WINAPI IsValidSid(PSID sid);
WINADVAPI BOOL  WINAPI EqualSid(PSID a, PSID b);
WINADVAPI DWORD WINAPI GetLengthSid(PSID sid);
WINADVAPI BOOL  WINAPI CopySid(DWORD n, PSID dst, PSID src);
WINADVAPI PSID_IDENTIFIER_AUTHORITY WINAPI GetSidIdentifierAuthority(PSID sid);
WINADVAPI PDWORD WINAPI GetSidSubAuthority(PSID sid, DWORD i);
WINADVAPI PUCHAR WINAPI GetSidSubAuthorityCount(PSID sid);
WINADVAPI BOOL  WINAPI ConvertSidToStringSidW(PSID sid, LPWSTR *out);
WINADVAPI BOOL  WINAPI ConvertSidToStringSidA(PSID sid, LPSTR *out);
WINADVAPI BOOL  WINAPI ConvertStringSidToSidW(LPCWSTR s, PSID *sid);
WINADVAPI BOOL  WINAPI LookupAccountSidW(LPCWSTR sys, PSID sid, LPWSTR name, LPDWORD nn, LPWSTR dom, LPDWORD nd, PSID_NAME_USE use);
WINADVAPI BOOL  WINAPI GetUserNameW(LPWSTR buf, LPDWORD n);
WINADVAPI BOOL  WINAPI GetUserNameA(LPSTR buf, LPDWORD n);
/* tokens */
WINADVAPI BOOL  WINAPI OpenProcessToken(HANDLE p, DWORD access, PHANDLE token);
WINADVAPI BOOL  WINAPI OpenThreadToken(HANDLE t, DWORD access, BOOL self, PHANDLE token);
WINADVAPI BOOL  WINAPI GetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS c, LPVOID buf, DWORD n, PDWORD ret);
WINADVAPI BOOL  WINAPI CheckTokenMembership(HANDLE token, PSID sid, PBOOL is_member);
WINADVAPI BOOL  WINAPI AdjustTokenPrivileges(HANDLE token, BOOL disable_all, PTOKEN_PRIVILEGES p, DWORD n, PTOKEN_PRIVILEGES prev, PDWORD ret);
WINADVAPI BOOL  WINAPI LookupPrivilegeValueW(LPCWSTR sys, LPCWSTR name, PLUID luid);
WINADVAPI BOOL  WINAPI LookupPrivilegeValueA(LPCSTR sys, LPCSTR name, PLUID luid);
WINADVAPI BOOL  WINAPI ImpersonateSelf(SECURITY_IMPERSONATION_LEVEL level);
WINADVAPI BOOL  WINAPI RevertToSelf(void);
WINADVAPI BOOL  WINAPI DuplicateToken(HANDLE t, SECURITY_IMPERSONATION_LEVEL l, PHANDLE out);
WINADVAPI BOOL  WINAPI DuplicateTokenEx(HANDLE t, DWORD access, LPSECURITY_ATTRIBUTES sa, SECURITY_IMPERSONATION_LEVEL l, int type, PHANDLE out);
WINADVAPI BOOL  WINAPI SetThreadToken(PHANDLE t, HANDLE token);
WINADVAPI BOOL  WINAPI ImpersonateLoggedOnUser(HANDLE t);
WINADVAPI BOOL  WINAPI CreateRestrictedToken(HANDLE t, DWORD flags, DWORD ndisable, PSID_AND_ATTRIBUTES disable, DWORD ndelete,
                                             PLUID_AND_ATTRIBUTES del, DWORD nrestrict, PSID_AND_ATTRIBUTES restricted, PHANDLE out);
WINADVAPI BOOL  WINAPI IsTokenRestricted(HANDLE t);
/* security descriptors and access checks */
WINADVAPI BOOL  WINAPI InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR sd, DWORD rev);
WINADVAPI BOOL  WINAPI SetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR sd, BOOL present, PACL acl, BOOL defaulted);
WINADVAPI BOOL  WINAPI MakeSelfRelativeSD(PSECURITY_DESCRIPTOR abs, PSECURITY_DESCRIPTOR rel, LPDWORD n);
WINADVAPI BOOL  WINAPI GetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR sd, PSID *o, LPBOOL def);
WINADVAPI BOOL  WINAPI GetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR sd, LPBOOL present, PACL *acl, LPBOOL defaulted);
WINADVAPI BOOL  WINAPI InitializeAcl(PACL acl, DWORD n, DWORD rev);
WINADVAPI BOOL  WINAPI AddAccessAllowedAce(PACL acl, DWORD rev, DWORD mask, PSID sid);
WINADVAPI BOOL  WINAPI MakeAbsoluteSD(PSECURITY_DESCRIPTOR rel, PSECURITY_DESCRIPTOR abs, LPDWORD abs_n, PACL dacl, LPDWORD dacl_n,
                                      PACL sacl, LPDWORD sacl_n, PSID owner, LPDWORD owner_n, PSID group, LPDWORD group_n);
WINADVAPI BOOL  WINAPI AddAccessDeniedAce(PACL acl, DWORD rev, DWORD mask, PSID sid);
WINADVAPI BOOL  WINAPI SetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR sd, PSID o, BOOL def);
WINADVAPI BOOL  WINAPI SetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR sd, PSID g, BOOL def);
WINADVAPI BOOL  WINAPI GetKernelObjectSecurity(HANDLE h, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd, DWORD n, LPDWORD need);
WINADVAPI BOOL  WINAPI SetKernelObjectSecurity(HANDLE h, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd);
WINADVAPI VOID  WINAPI MapGenericMask(PDWORD mask, PGENERIC_MAPPING m);
WINADVAPI BOOL  WINAPI AccessCheck(PSECURITY_DESCRIPTOR sd, HANDLE token, DWORD want, PGENERIC_MAPPING m, PPRIVILEGE_SET ps,
                                   LPDWORD psn, LPDWORD granted, LPBOOL status);
WINADVAPI DWORD WINAPI GetNamedSecurityInfoW(LPCWSTR name, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID *owner, PSID *group,
                                             PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *sd);
WINADVAPI DWORD WINAPI SetNamedSecurityInfoW(LPWSTR name, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID owner, PSID group,
                                             PACL dacl, PACL sacl);
WINADVAPI BOOL  WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorW(LPCWSTR s, DWORD rev, PSECURITY_DESCRIPTOR *sd, PULONG n);
WINADVAPI BOOL  WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorA(LPCSTR s, DWORD rev, PSECURITY_DESCRIPTOR *sd, PULONG n);
WINADVAPI BOOL  WINAPI ConvertSecurityDescriptorToStringSecurityDescriptorW(PSECURITY_DESCRIPTOR sd, DWORD rev, SECURITY_INFORMATION si,
                                                                          LPWSTR *out, PULONG n);
WINADVAPI BOOL  WINAPI ConvertSecurityDescriptorToStringSecurityDescriptorA(PSECURITY_DESCRIPTOR sd, DWORD rev, SECURITY_INFORMATION si,
                                                                          LPSTR *out, PULONG n);
WINADVAPI BOOL  WINAPI LookupAccountNameW(LPCWSTR sys, LPCWSTR name, PSID sid, LPDWORD ns, LPWSTR dom, LPDWORD nd, PSID_NAME_USE use);
WINADVAPI BOOL  WINAPI LookupAccountNameA(LPCSTR sys, LPCSTR name, PSID sid, LPDWORD ns, LPSTR dom, LPDWORD nd, PSID_NAME_USE use);
WINADVAPI BOOL  WINAPI IsValidSecurityDescriptor(PSECURITY_DESCRIPTOR sd);
WINADVAPI BOOL  WINAPI IsValidAcl(PACL acl);
WINADVAPI BOOL  WINAPI GetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR sd, PSID *g, LPBOOL def);
WINADVAPI BOOL  WINAPI GetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR sd, LPBOOL present, PACL *acl, LPBOOL defaulted);
WINADVAPI BOOL  WINAPI SetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR sd, BOOL present, PACL acl, BOOL defaulted);
WINADVAPI BOOL  WINAPI GetSecurityDescriptorControl(PSECURITY_DESCRIPTOR sd, PSECURITY_DESCRIPTOR_CONTROL c, LPDWORD rev);
WINADVAPI BOOL  WINAPI SetSecurityDescriptorControl(PSECURITY_DESCRIPTOR sd, SECURITY_DESCRIPTOR_CONTROL mask, SECURITY_DESCRIPTOR_CONTROL set);
WINADVAPI DWORD WINAPI GetSecurityDescriptorLength(PSECURITY_DESCRIPTOR sd);
WINADVAPI BOOL  WINAPI GetAce(PACL acl, DWORD i, LPVOID *ace);
#define SACL_SECURITY_INFORMATION  0x00000008
#define LABEL_SECURITY_INFORMATION 0x00000010
#define SE_SACL_PRESENT          0x0010
#define SE_DACL_AUTO_INHERIT_REQ 0x0100
#define SE_SACL_AUTO_INHERIT_REQ 0x0200
#define SE_DACL_AUTO_INHERITED   0x0400
#define SE_SACL_AUTO_INHERITED   0x0800
#define SE_DACL_PROTECTED        0x1000
#define SE_SACL_PROTECTED        0x2000
#define ERROR_INVALID_ACL            1336
#define ERROR_INVALID_SECURITY_DESCR 1338
#define ERROR_NONE_MAPPED            1332
#define ERROR_UNKNOWN_REVISION       1305
#define PROTECTED_DACL_SECURITY_INFORMATION   0x80000000
#define UNPROTECTED_DACL_SECURITY_INFORMATION 0x20000000
#define SDDL_REVISION_1          1

/* aclapi: access lists from EXPLICIT_ACCESS entries */
typedef enum { NOT_USED_ACCESS, GRANT_ACCESS, SET_ACCESS, DENY_ACCESS, REVOKE_ACCESS, SET_AUDIT_SUCCESS,
               SET_AUDIT_FAILURE } ACCESS_MODE;
typedef enum { NO_MULTIPLE_TRUSTEE, TRUSTEE_IS_IMPERSONATE } MULTIPLE_TRUSTEE_OPERATION;
typedef enum { TRUSTEE_IS_SID, TRUSTEE_IS_NAME, TRUSTEE_BAD_FORM, TRUSTEE_IS_OBJECTS_AND_SID,
               TRUSTEE_IS_OBJECTS_AND_NAME } TRUSTEE_FORM;
typedef enum { TRUSTEE_IS_UNKNOWN, TRUSTEE_IS_USER, TRUSTEE_IS_GROUP, TRUSTEE_IS_DOMAIN, TRUSTEE_IS_ALIAS,
               TRUSTEE_IS_WELL_KNOWN_GROUP, TRUSTEE_IS_DELETED, TRUSTEE_IS_INVALID, TRUSTEE_IS_COMPUTER } TRUSTEE_TYPE;
typedef struct _TRUSTEE_W {
    struct _TRUSTEE_W *pMultipleTrustee;
    MULTIPLE_TRUSTEE_OPERATION MultipleTrusteeOperation;
    TRUSTEE_FORM TrusteeForm;
    TRUSTEE_TYPE TrusteeType;
    LPWSTR ptstrName;
} TRUSTEE_W, *PTRUSTEE_W;
typedef struct _TRUSTEE_A {
    struct _TRUSTEE_A *pMultipleTrustee;
    MULTIPLE_TRUSTEE_OPERATION MultipleTrusteeOperation;
    TRUSTEE_FORM TrusteeForm;
    TRUSTEE_TYPE TrusteeType;
    LPSTR ptstrName;
} TRUSTEE_A, *PTRUSTEE_A;
typedef struct _EXPLICIT_ACCESS_W {
    DWORD grfAccessPermissions;
    ACCESS_MODE grfAccessMode;
    DWORD grfInheritance;
    TRUSTEE_W Trustee;
} EXPLICIT_ACCESS_W, *PEXPLICIT_ACCESS_W;
typedef struct _EXPLICIT_ACCESS_A {
    DWORD grfAccessPermissions;
    ACCESS_MODE grfAccessMode;
    DWORD grfInheritance;
    TRUSTEE_A Trustee;
} EXPLICIT_ACCESS_A, *PEXPLICIT_ACCESS_A;
#define NO_INHERITANCE                     0x0
#define SUB_OBJECTS_ONLY_INHERIT           0x1
#define SUB_CONTAINERS_ONLY_INHERIT        0x2
#define SUB_CONTAINERS_AND_OBJECTS_INHERIT 0x3
#define INHERIT_NO_PROPAGATE               0x4
#define INHERIT_ONLY                       0x8
WINADVAPI DWORD WINAPI SetEntriesInAclW(ULONG n, PEXPLICIT_ACCESS_W entries, PACL old, PACL *out);
WINADVAPI DWORD WINAPI SetEntriesInAclA(ULONG n, PEXPLICIT_ACCESS_A entries, PACL old, PACL *out);
WINADVAPI DWORD WINAPI GetExplicitEntriesFromAclW(PACL acl, PULONG n, PEXPLICIT_ACCESS_W *entries);
WINADVAPI void  WINAPI BuildTrusteeWithSidW(PTRUSTEE_W t, PSID sid);
WINADVAPI void  WINAPI BuildTrusteeWithNameW(PTRUSTEE_W t, LPWSTR name);
WINADVAPI void  WINAPI BuildExplicitAccessWithNameW(PEXPLICIT_ACCESS_W ea, LPWSTR name, DWORD perms, ACCESS_MODE mode, DWORD inherit);
WINADVAPI DWORD WINAPI BuildSecurityDescriptorW(PTRUSTEE_W owner, PTRUSTEE_W group, ULONG n, PEXPLICIT_ACCESS_W access,
                                                ULONG naudit, PEXPLICIT_ACCESS_W audit, PSECURITY_DESCRIPTOR old, PULONG size,
                                                PSECURITY_DESCRIPTOR *out);

/* random numbers, hashes (CryptoAPI) */
WINADVAPI BOOLEAN WINAPI SystemFunction036(PVOID buf, ULONG n);
#define RtlGenRandom SystemFunction036
WINADVAPI BOOL  WINAPI CryptAcquireContextW(HCRYPTPROV *p, LPCWSTR c, LPCWSTR prov, DWORD type, DWORD flags);
WINADVAPI BOOL  WINAPI CryptAcquireContextA(HCRYPTPROV *p, LPCSTR c, LPCSTR prov, DWORD type, DWORD flags);
WINADVAPI BOOL  WINAPI CryptReleaseContext(HCRYPTPROV p, DWORD flags);
WINADVAPI BOOL  WINAPI CryptGenRandom(HCRYPTPROV p, DWORD n, BYTE *buf);
WINADVAPI BOOL  WINAPI CryptCreateHash(HCRYPTPROV p, DWORD alg, HCRYPTKEY key, DWORD flags, HCRYPTHASH *h);
WINADVAPI BOOL  WINAPI CryptHashData(HCRYPTHASH h, const BYTE *data, DWORD n, DWORD flags);
WINADVAPI BOOL  WINAPI CryptGetHashParam(HCRYPTHASH h, DWORD param, BYTE *out, DWORD *n, DWORD flags);
WINADVAPI BOOL  WINAPI CryptDestroyHash(HCRYPTHASH h);

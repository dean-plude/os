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
typedef struct _TOKEN_ELEVATION { DWORD TokenIsElevated; } TOKEN_ELEVATION;
typedef enum { TokenUser = 1, TokenGroups, TokenPrivileges, TokenOwner, TokenPrimaryGroup, TokenDefaultDacl, TokenSource,
               TokenType, TokenImpersonationLevel, TokenStatistics, TokenRestrictedSids, TokenSessionId,
               TokenGroupsAndPrivileges, TokenSessionReference, TokenSandBoxInert, TokenAuditPolicy, TokenOrigin,
               TokenElevationType, TokenLinkedToken, TokenElevation, TokenHasRestrictions, TokenAccessInformation,
               TokenVirtualizationAllowed, TokenVirtualizationEnabled, TokenIntegrityLevel, TokenUIAccess,
               TokenMandatoryPolicy, TokenLogonSid, TokenIsAppContainer } TOKEN_INFORMATION_CLASS;
typedef enum { SecurityAnonymous, SecurityIdentification, SecurityImpersonation, SecurityDelegation } SECURITY_IMPERSONATION_LEVEL;
typedef enum { SidTypeUser = 1, SidTypeGroup, SidTypeDomain, SidTypeAlias, SidTypeWellKnownGroup, SidTypeDeletedAccount,
               SidTypeInvalid, SidTypeUnknown, SidTypeComputer, SidTypeLabel } SID_NAME_USE, *PSID_NAME_USE;
typedef enum { SE_UNKNOWN_OBJECT_TYPE, SE_FILE_OBJECT, SE_SERVICE, SE_PRINTER, SE_REGISTRY_KEY } SE_OBJECT_TYPE;

#define TOKEN_QUERY              0x0008
#define TOKEN_ADJUST_PRIVILEGES  0x0020
#define TOKEN_DUPLICATE          0x0002
#define TOKEN_IMPERSONATE        0x0004
#define TOKEN_ALL_ACCESS         0xF01FF
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
/* security descriptors and access checks */
WINADVAPI BOOL  WINAPI InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR sd, DWORD rev);
WINADVAPI BOOL  WINAPI SetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR sd, BOOL present, PACL acl, BOOL defaulted);
WINADVAPI BOOL  WINAPI InitializeAcl(PACL acl, DWORD n, DWORD rev);
WINADVAPI BOOL  WINAPI AddAccessAllowedAce(PACL acl, DWORD rev, DWORD mask, PSID sid);
WINADVAPI VOID  WINAPI MapGenericMask(PDWORD mask, PGENERIC_MAPPING m);
WINADVAPI BOOL  WINAPI AccessCheck(PSECURITY_DESCRIPTOR sd, HANDLE token, DWORD want, PGENERIC_MAPPING m, PPRIVILEGE_SET ps,
                                   LPDWORD psn, LPDWORD granted, LPBOOL status);
WINADVAPI DWORD WINAPI GetNamedSecurityInfoW(LPCWSTR name, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID *owner, PSID *group,
                                             PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *sd);
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

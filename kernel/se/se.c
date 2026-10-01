/*
 * se.c — NT Security Reference Monitor
 *
 * Phase 2 implementation:
 *   - Well-known SIDs (SYSTEM, Administrators, Everyone, LocalService,
 *     NetworkService)
 *   - System token (all privileges enabled, used by PsInitialSystemProcess)
 *   - Token duplication
 *   - Stub access check (grants all access in Phase 2 — full evaluation
 *     requires a complete DACL traversal added in Phase 3)
 */

#include "se.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../ps/ps.h"
#include "../include/types.h"

/* -----------------------------------------------------------------------
 * Well-known SID pointers
 * ----------------------------------------------------------------------- */
PSID SeWorldSid;
PSID SeLocalSystemSid;
PSID SeAdminsSid;
PSID SeLocalServiceSid;
PSID SeNetworkServiceSid;

/* Storage for the well-known SIDs */
static SID se_world_sid;
static SID se_system_sid;
static SID se_admins_sid;
static SID se_local_service_sid;
static SID se_network_service_sid;

/* -----------------------------------------------------------------------
 * LUID counter for token IDs
 * ----------------------------------------------------------------------- */
static volatile UINT32 se_next_token_id = 1;

static LUID se_alloc_luid(void)
{
    LUID l;
    l.LowPart  = __atomic_fetch_add(&se_next_token_id, 1, __ATOMIC_SEQ_CST);
    l.HighPart = 0;
    return l;
}

/* -----------------------------------------------------------------------
 * SID helpers
 * ----------------------------------------------------------------------- */

/* Build SID directly from components (Phase 2 simplification: max 2 subs) */
static void se_build_sid(SID *sid,
                          UINT8 auth5,
                          UINT8 sub_count,
                          UINT32 sub0,
                          UINT32 sub1)
{
    sid->Revision = SID_REVISION;
    sid->SubAuthorityCount = sub_count;
    sid->IdentifierAuthority.Value[0] = 0;
    sid->IdentifierAuthority.Value[1] = 0;
    sid->IdentifierAuthority.Value[2] = 0;
    sid->IdentifierAuthority.Value[3] = 0;
    sid->IdentifierAuthority.Value[4] = 0;
    sid->IdentifierAuthority.Value[5] = auth5;
    if (sub_count > 0) sid->SubAuthority[0] = sub0;
    if (sub_count > 1) sid->SubAuthority[1] = sub1;
}

static bool se_sids_equal(PSID a, PSID b)
{
    if (a->Revision != b->Revision) return false;
    if (a->SubAuthorityCount != b->SubAuthorityCount) return false;
    for (int i = 0; i < 6; i++)
        if (a->IdentifierAuthority.Value[i] != b->IdentifierAuthority.Value[i])
            return false;
    for (UINT8 i = 0; i < a->SubAuthorityCount; i++)
        if (a->SubAuthority[i] != b->SubAuthority[i]) return false;
    return true;
}

/* -----------------------------------------------------------------------
 * SeInitialize
 * ----------------------------------------------------------------------- */
void SeInitialize(void)
{
    /* S-1-1-0 — Everyone */
    se_build_sid(&se_world_sid, 1, 1, 0, 0);
    SeWorldSid = &se_world_sid;

    /* S-1-5-18 — NT AUTHORITY\SYSTEM */
    se_build_sid(&se_system_sid, 5, 1, SECURITY_LOCAL_SYSTEM_RID, 0);
    SeLocalSystemSid = &se_system_sid;

    /* S-1-5-32-544 — BUILTIN\Administrators */
    se_build_sid(&se_admins_sid, 5, 2,
                  SECURITY_BUILTIN_DOMAIN_RID,
                  DOMAIN_ALIAS_RID_ADMINS);
    SeAdminsSid = &se_admins_sid;

    /* S-1-5-19 — NT AUTHORITY\Local Service */
    se_build_sid(&se_local_service_sid, 5, 1, SECURITY_LOCAL_SERVICE_RID, 0);
    SeLocalServiceSid = &se_local_service_sid;

    /* S-1-5-20 — NT AUTHORITY\Network Service */
    se_build_sid(&se_network_service_sid, 5, 1, SECURITY_NETWORK_SERVICE_RID, 0);
    SeNetworkServiceSid = &se_network_service_sid;

    kprintf("[SE] Security Reference Monitor initialized\n");
    kprintf("[SE] Well-known SIDs: SYSTEM S-1-5-18, Admins S-1-5-32-544, "
            "Everyone S-1-1-0\n");
}

/* -----------------------------------------------------------------------
 * SeCreateSystemToken
 * Creates the omnipotent token used by the System process.
 * All privileges are enabled; user = SYSTEM.
 * ----------------------------------------------------------------------- */
NTSTATUS SeCreateSystemToken(PTOKEN *TokenOut)
{
    PTOKEN tok = kzalloc(sizeof(TOKEN));
    if (!tok) return STATUS_NO_MEMORY;

    tok->TokenId             = se_alloc_luid();
    tok->AuthenticationId    = (LUID){ 0x3E7, 0 };  /* SYSTEM_LUID */
    tok->TokenType           = TOKEN_TYPE_PRIMARY;
    tok->ImpersonationLevel  = 0;

    /* User = SYSTEM (index 0) */
    tok->InlineSids[0]          = se_system_sid;
    tok->InlineSids[1]          = se_admins_sid;
    tok->InlineSidCount         = 2;

    tok->UserAndGroups[0].Sid        = &tok->InlineSids[0];
    tok->UserAndGroups[0].Attributes = 0;

    tok->UserAndGroups[1].Sid        = &tok->InlineSids[1];
    tok->UserAndGroups[1].Attributes = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT
                                     | SE_GROUP_MANDATORY;
    tok->UserAndGroupCount = 2;

    /* Enable all privileges for the System token */
    tok->PrivilegeCount = 0;
    for (UINT32 p = SE_MIN_WELL_KNOWN_PRIVILEGE;
         p <= SE_MAX_WELL_KNOWN_PRIVILEGE &&
         tok->PrivilegeCount < TOKEN_PRIVILEGES_MAX; p++)
    {
        tok->Privileges[tok->PrivilegeCount].Luid       = LUID_FROM_PRIVILEGE(p);
        tok->Privileges[tok->PrivilegeCount].Attributes =
            SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT;
        tok->PrivilegeCount++;
    }

    tok->DefaultDacl = NULL;  /* NULL DACL = grant all in Phase 2 */

    if (TokenOut) *TokenOut = tok;
    kprintf("[SE] System token created (ID=%lu, %u privileges)\n",
            tok->TokenId.LowPart, tok->PrivilegeCount);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * SeDuplicateToken
 * ----------------------------------------------------------------------- */
NTSTATUS SeDuplicateToken(PTOKEN SourceToken, UINT32 TokenType,
                           PTOKEN *NewToken)
{
    PTOKEN tok = kmalloc(sizeof(TOKEN));
    if (!tok) return STATUS_NO_MEMORY;

    __builtin_memcpy(tok, SourceToken, sizeof(TOKEN));
    tok->TokenId    = se_alloc_luid();
    tok->TokenType  = TokenType;

    /* Fix up inlined SID pointers after memcpy (they pointed into SourceToken) */
    for (UINT32 i = 0; i < tok->UserAndGroupCount; i++) {
        /* Check if the Sid was pointing at SourceToken->InlineSids */
        for (UINT32 j = 0; j < SourceToken->InlineSidCount; j++) {
            if (tok->UserAndGroups[i].Sid == &SourceToken->InlineSids[j]) {
                tok->UserAndGroups[i].Sid = &tok->InlineSids[j];
            }
        }
    }

    if (NewToken) *NewToken = tok;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * SeAccessCheck
 *
 * The DACL decides, as on Windows: no DACL grants everything, an empty one
 * nothing; otherwise the ACEs are taken in order, and for each one whose
 * SID the token holds (enabled), allowed rights are granted unless an
 * earlier ACE denied them and denied rights refused unless an earlier ACE
 * granted them.  The owner always may read and change the DACL.  The
 * SYSTEM token (the kernel's own threads) is not checked.
 * ----------------------------------------------------------------------- */
#define SE_READ_CONTROL      0x00020000u
#define SE_WRITE_DAC         0x00040000u
#define SE_MAXIMUM_ALLOWED   0x02000000u
#define SE_GENERIC_ALL       0x10000000u

static bool token_has_sid(PTOKEN Token, PSID Sid)
{
    for (UINT32 i = 0; i < Token->UserAndGroupCount; i++) {
        if (i > 0 && !(Token->UserAndGroups[i].Attributes & SE_GROUP_ENABLED)) continue;
        if (Token->UserAndGroups[i].Sid && se_sids_equal(Token->UserAndGroups[i].Sid, Sid)) return true;
    }
    return false;
}

bool SeAccessCheck(
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    PTOKEN               Token,
    ACCESS_MASK          DesiredAccess,
    ACCESS_MASK         *GrantedAccess)
{
    SECURITY_DESCRIPTOR *sd = (SECURITY_DESCRIPTOR *)SecurityDescriptor;
    bool max = (DesiredAccess & SE_MAXIMUM_ALLOWED) != 0;
    ACCESS_MASK want = DesiredAccess & ~SE_MAXIMUM_ALLOWED;

    /* No SD, no token, the SYSTEM token, or no DACL: everything */
    if (!sd || !Token ||
        (Token->UserAndGroupCount > 0 &&
         se_sids_equal(Token->UserAndGroups[0].Sid, SeLocalSystemSid)) ||
        !(sd->Control & SE_DACL_PRESENT) || !sd->Dacl)
    {
        if (GrantedAccess) *GrantedAccess = max ? 0x1FFFFFu | want : want;   /* (all specific and standard rights) */
        return true;
    }

    ACCESS_MASK allowed = 0, denied = 0;
    if (sd->Owner && token_has_sid(Token, sd->Owner)) allowed = SE_READ_CONTROL | SE_WRITE_DAC;
    const ACL *acl = sd->Dacl;
    const UINT8 *p = (const UINT8 *)acl + sizeof(ACL), *end = (const UINT8 *)acl + acl->AclSize;
    for (UINT16 i = 0; i < acl->AceCount; i++) {
        const ACE_HEADER *h = (const ACE_HEADER *)p;
        if (p + sizeof(ACE_HEADER) > end || h->AceSize < sizeof(ACCESS_ALLOWED_ACE) || p + h->AceSize > end) break;
        p += h->AceSize;
        if (h->AceFlags & INHERIT_ONLY_ACE) continue;
        if (h->AceType != ACCESS_ALLOWED_ACE_TYPE && h->AceType != ACCESS_DENIED_ACE_TYPE) continue;
        const ACCESS_ALLOWED_ACE *ace = (const ACCESS_ALLOWED_ACE *)h;
        PSID sid = (PSID)&ace->SidStart;
        if (8u + 4u * sid->SubAuthorityCount > h->AceSize - 8u || !token_has_sid(Token, sid)) continue;
        ACCESS_MASK m = ace->Mask & SE_GENERIC_ALL ? 0x1FFFFFu : ace->Mask;
        if (h->AceType == ACCESS_ALLOWED_ACE_TYPE) allowed |= m & ~denied;
        else denied |= m & ~allowed;
    }
    if (want & ~allowed) {
        if (GrantedAccess) *GrantedAccess = 0;
        return false;
    }
    if (GrantedAccess) *GrantedAccess = max ? allowed | want : want;
    return true;
}

/* -----------------------------------------------------------------------
 * SePrivilegeCheck
 * ----------------------------------------------------------------------- */
bool SePrivilegeCheck(PTOKEN Token, UINT32 PrivilegeValue)
{
    if (!Token) return false;
    LUID req = LUID_FROM_PRIVILEGE(PrivilegeValue);
    for (UINT32 i = 0; i < Token->PrivilegeCount; i++) {
        if (Token->Privileges[i].Luid.LowPart  == req.LowPart &&
            Token->Privileges[i].Luid.HighPart == req.HighPart)
        {
            return (Token->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) != 0;
        }
    }
    return false;
}

/* -----------------------------------------------------------------------
 * SeDereferenceToken
 * ----------------------------------------------------------------------- */
void SeDereferenceToken(PTOKEN Token)
{
    /* Phase 2: tokens are not reference-counted yet; caller must not
     * free the system token.  Phase 3 adds ref-counting via ObXxx. */
    (void)Token;
}

/* -----------------------------------------------------------------------
 * SeGetCurrentToken
 * ----------------------------------------------------------------------- */
PTOKEN SeGetCurrentToken(void)
{
    PEPROCESS p = PsGetCurrentProcess();
    if (!p) return NULL;
    return (PTOKEN)p->Token;
}

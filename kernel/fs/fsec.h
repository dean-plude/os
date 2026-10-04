/*
 * fsec.h — file security on drive C:
 *
 * A node may carry a security descriptor (RamNode.sd, self-relative).  One
 * without inherits from the nearest directory above that has one: the
 * ACEs it passes to files (OBJECT_INHERIT) or directories
 * (CONTAINER_INHERIT; a directory shows the ones meant only for files as
 * inherit-only), with CREATOR OWNER standing for the node's owner, who is
 * the user.  A DACL keeps its SE_DACL_PROTECTED and SE_DACL_AUTO_INHERITED
 * bits; advapi32's SetNamedSecurityInfo merges a folder's inheritable
 * entries into an unprotected DACL, as Windows does.  With no descriptor anywhere above, a node has no DACL
 * and everyone may do anything, as on FAT.
 *
 * Access is checked as the calling thread's effective token (its
 * impersonation token, otherwise its process's; kernel/um/um_security.c),
 * so restricted and impersonation tokens are refused what their SIDs are
 * not granted.  The owner may always read and change the DACL.
 */

#pragma once

#include "../include/types.h"
#include "ramfs.h"

#define FSEC_MAXIMUM_ALLOWED  0x02000000u
#define FSEC_DELETE           0x00010000u
#define FSEC_DELETE_CHILD     0x00000040u
#define FSEC_ADD_FILE         0x00000002u
#define FSEC_ADD_SUBDIRECTORY 0x00000004u

/* The rights of @want (generic rights mapped as for files; MAXIMUM_ALLOWED
 * asks for whatever the DACL grants) the calling thread has to @n.  False if one is
 * denied; *granted (may be NULL) receives what was granted. */
bool   FsecAccess(RamNode *n, UINT32 want, UINT32 *granted);

/* @n's descriptor with the parts @info asks for (OWNER 1, GROUP 2, DACL 4;
 * a SACL is not kept) as a self-relative descriptor in @out; returns its
 * length, which may be more than @cap (then nothing is written). */
UINT32 FsecQuery(RamNode *n, UINT32 info, void *out, UINT32 cap);

/* Replace the parts @info names of @n's descriptor with those of @sd
 * (self-relative, @len bytes).  False if @sd is not valid. */
bool   FsecSet(RamNode *n, UINT32 info, const void *sd, UINT32 len);

/* Whether @sd (@len bytes) is a valid self-relative descriptor; its real length in *used */
bool   FsecValid(const void *sd, UINT32 len, UINT32 *used);

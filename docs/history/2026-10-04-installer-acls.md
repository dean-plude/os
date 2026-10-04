## Installer ACLs: the VC++ Redistributable installs its first package

The Visual C++ Redistributable's installer (WiX Burn, which GOG GALAXY
needs for `mfc140u.dll`) started its elevated engine and then failed
0x80070005 creating its folder under `C:\ProgramData\Package Cache`.
Burn secures the cache with an access list it builds with
`SetEntriesInAcl` (Administrators and SYSTEM full control, Everyone and
Users read, inherited by everything below), and NovaOS's
`SetEntriesInAcl` returned an empty list: nobody, the elevated engine
included, could add anything to the folder.  NovaOS now builds and keeps
these access lists as Windows does, and checks them as before.

- **`SetEntriesInAcl`** (`advapi32/aclapi.c`) merges its entries into a
  copy of the old list: `GRANT_ACCESS` adds rights to the trustee's allow
  entry, `SET_ACCESS` replaces what the trustee had, `DENY_ACCESS` adds a
  deny entry, `REVOKE_ACCESS` removes the trustee's entries, and the
  result is in canonical order (denies, allows, then the old inherited
  entries).  Trustees are SIDs, account names or `CURRENT_USER`.  Also
  `GetExplicitEntriesFromAcl`, `BuildSecurityDescriptor` (which kept
  nothing before) and the trustee helpers; the types are in `winsec.h`.
- **SDDL** (`advapi32/sddl.c`): `ConvertStringSecurityDescriptorToSecurityDescriptor`
  parses owner, group, DACL and SACL with their flags, ACE types, flags,
  rights by number or name, object GUIDs and SID aliases; it used to
  return a descriptor that let everyone in, whatever the text said.
  `ConvertSecurityDescriptorToStringSecurityDescriptor` writes the real
  descriptor back as text.  The SID aliases are complete (`ME` is the
  medium integrity level, not the user).
- **`LookupAccountName`** finds the account a name names (the user, with
  or without `NOVAOS\` or the computer name, the built-in groups and
  well-known accounts) and fails with `ERROR_NONE_MAPPED` for a name
  that is no account, where it used to answer with the user.
- **Inheritance on `SetNamedSecurityInfo`**: with
  `UNPROTECTED_DACL_SECURITY_INFORMATION` (or neither flag on a DACL that
  was not protected), the folder's inheritable entries are merged in
  after the explicit ones, as Windows' automatic inheritance does.  Burn
  "resets" each cached folder that way: an empty DACL that inherits the
  cache root's entries.  `PROTECTED_DACL_SECURITY_INFORMATION` keeps a
  DACL as given, and the kernel now keeps the protected and
  auto-inherited bits with a file's DACL.  The security calls open files
  with only the right they need (as Windows does), so an owner whose DACL
  grants nothing can still change it.
- **Kernel**: a folder shows an entry its parent passes only to files
  (`OBJECT_INHERIT` without `CONTAINER_INHERIT`) as inherit-only, where it
  used to leave it out.
- **Self-test** `acltest` (core 070, 64- and 32-bit, 112 checks): the
  above, and Burn's cache: the root secured by its owner, a folder in it
  refused to us and made by the elevated (linked) token, reset to inherit,
  a file cached in it that we can read and not write.

**Where the Visual C++ Redistributable stops now**: its elevated engine
caches the bundle, registers it and installs the Minimum Runtime MSI
(`vcruntime140.dll`, `msvcp140.dll` and the rest), then crashes calling
`msi.dll`'s `MsiSourceListAddSourceExW`, which NovaOS does not have (Burn
looks it up and calls it without checking), so the Additional Runtime
with MFC is not installed yet.

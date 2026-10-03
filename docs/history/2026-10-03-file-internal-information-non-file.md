## FileInternalInformation on a non-file handle

The "App corpus" check failed on pull requests (and would have failed
nightly): the first real ripgrep search died with a kernel page fault in
`RamfsFileId`, called from `UmSyscall` with a null node (CR2 0x170).
ripgrep asks `NtQueryInformationFile` for `FileInternalInformation` on its
standard handles, and for a console or pipe handle there is no `RamNode`, so
the dereference of `h->node` faulted and halted the VM; every later program
in the run then reported as not run (0 of 14).

`FileInternalInformation` now returns the node pointer as the file ID only
for file and directory handles and 0 for others, as `FileStatInformation`
already did.  No corpus entry was changed or removed, and the three required
check names are unchanged.

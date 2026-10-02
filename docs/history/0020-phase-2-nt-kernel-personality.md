## Phase 2 — NT Kernel Personality
- Object Manager (`ObCreateObject`, `ObReferenceObject`, `ObDereferenceObject`, handle tables)
- Security Reference Monitor (`SeInitialize`, `SeCreateSystemToken`, ACL/ACE stubs)
- Process Manager (`EPROCESS`/`ETHREAD`/`KPROCESS`/`KTHREAD`, `PsCreateSystemProcess`, `PsCreateSystemThread`)
- Configuration Manager (registry hive bootstrap, `CmInitialize`)
- Syscall dispatcher: INT 0x2E (legacy) + SYSCALL/SYSRET MSR path (`KiSystemCall64`)
- NT syscall table with ~40 entries (NtAllocateVirtualMemory, NtCreateFile, etc.)

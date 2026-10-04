## Registry keys under many threads and processes

While Steam's service restarted itself hundreds of times (its service
manager start times out, `ERROR_SERVICE_REQUEST_TIMEOUT`), one local run of
the Steam corpus test stopped on a kernel page fault: closing a registry key
handle freed the key's values, and one value's data pointer was a physical
page address (`kfree` on `0x992b000`, from `key_ob_destroy`).  The value's
memory had been handed to something else while the key still listed it:
the fields matched the record the kernel keeps for a shared program-image
page (its frame at the same offset as a value's data), so a 64-byte heap
block was in two hands at once.

What was checked: a kernel built with poisoned, quarantined heap blocks
(double frees, writes after free and damaged free lists reported with who
freed the block) and poisoned free pages ran the Steam test four more times
(over a thousand service restarts) and `regtest` (below) with thousands of
key and value races across 17 processes, and caught nothing; the registry's
reference counts, handle table and locks hold up on reading and under
test.  Where the block went twice is not known yet; the next report of
it starts from here.

Two real races in the registry were found and fixed on the way:

- **The kernel's own registry reads** (the keyboard layout, display
  settings, the update channel: `um_registry_get_dword`, `_sz`, `_bin`)
  read a value without the key's lock, so a program changing or deleting
  that value at the same moment could have its old data freed under the
  read.  They now take the key's lock, as programs' reads do.
- **A synchronous change wait** (`RegNotifyChangeKeyValue` without an
  event, `NtNotifyChangeKey` waiting) looked for its watch in the list by
  address after waking; once the watch had fired and been freed, another
  thread's new watch could have the same address and be dropped instead,
  so that thread never heard of its change.  A waiting caller now frees
  its own watch, so its address cannot be reused while it looks.

`regtest` (64- and 32-bit, core self-test) runs six threads and four waves
of four child processes on the same keys: opening and closing a shared
key, creating and deleting subkeys, deleting keys others hold open,
duplicating key handles (into a child too), setting, reading and deleting
one key's values from every process at once (as the service control
manager's state values are), change notifications left pending on closed
keys and children ending with keys and watches open, and a thread waiting
synchronously for changes meanwhile.

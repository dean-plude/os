- **Program support**: the PE loader with TLS, `DllMain`, forwarders and
  API sets; x64 and x86 structured exceptions, with Windows'
  alignment-fault fixup for misaligned SSE moves; Windows' segment
  selectors, so 64-bit programs can far-jump into 32-bit code; registry saved to disk;
  COM in-process servers and type libraries; drag and drop; a shared clipboard; `.lnk`
  shortcuts; Windows Installer packages; services (`advapi32`'s service
  control manager); scheduled tasks (Task Scheduler 2.0, kept in
  `C:\Windows\System32\Tasks`); the Data Protection API.

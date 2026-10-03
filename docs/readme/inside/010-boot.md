- **Boot**: a UEFI bootloader (a PE32+ EFI application) loads the ELF kernel
  from the EFI System Partition, or from the ISO on a CD or written to a
  USB stick (the installation media, run live; started from a stick,
  NovaOS writes its log into `\EFI\NOVA\bootlog.txt` on it).

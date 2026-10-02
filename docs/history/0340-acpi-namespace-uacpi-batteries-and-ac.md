## ACPI namespace (uACPI): batteries and AC power

- **AML interpreter**: uACPI 6.1 (`third_party/uacpi`, MIT) loads the DSDT
  and SSDTs.  `kernel/hal/aml.c` gives it memory, ports, PCI configuration
  space, time, locks and a work queue.  It runs on an `acpi` kernel thread
  that starts after boot and then polls the SCI every 100 ms (device
  interrupts stay off in NovaOS), running GPE methods and `Notify`
  handlers.
- **Batteries and AC adapters** (`PNP0C0A`, `ACPI0003`): `_BIX` or `_BIF`,
  `_BST` and `_PSR`, read every 5 s and on notifications, converted to
  mWh/mW.  `GetSystemPowerStatus` (kernel32), `CallNtPowerInformation`
  (`SystemBatteryState`) and `GetPwrCapabilities` report them through the
  new `NtPowerInformation`.  `battery.exe` prints them.
- **Power buttons**: once the namespace is loaded, the fixed button goes
  through uACPI and control-method buttons (`PNP0C0C`, `Notify 0x80`)
  work too.
- **Sleep**: `_PTS` and `_WAK` now run around S3, with only wake GPEs on
  while asleep.
- Tested in QEMU with extra SSDTs (`-acpitable`) describing a battery in
  mWh on battery power (75%, 3 h left) and one in mAh charging on AC
  (25%); sleep and the power button pass as before.

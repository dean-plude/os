## ACPI power: shut down, restart, power button

- **`kernel/hal/acpi.c`** reads the FADT (PM1 event and control blocks,
  the reset register, hardware-reduced sleep registers) and finds the S5
  sleep type in the `\_S5` package of the DSDT or an SSDT, which is plain
  data, so no AML interpreter is needed.  It switches the chipset into ACPI
  mode through `SMI_CMD` and enables the fixed-feature power button.
  The MADT lookup in `smp.c` now goes through `AcpiFindTable`.
- **Shut down** writes `SLP_TYP | SLP_EN` to PM1a/PM1b control, falling
  back to the virtual machines' ports.  **Restart** uses the FADT reset
  register, then port `0xCF9`, then the 8042, then a triple fault.  Both
  save drive C: first and show a "Shutting down" / "Restarting" screen.
- **Power button**: the desktop polls `PWRBTN_STS` and shuts down, as
  Windows does by default (`system_powerdown` in the QEMU monitor).
- **Programs**: `NtShutdownSystem`, `ExitWindowsEx` (shut down, power off,
  restart; no log-off), `InitiateSystemShutdown[Ex]` (at once; there is no
  countdown to abort), and a `shutdown.exe` (`/s`, `/p`, `/r`).

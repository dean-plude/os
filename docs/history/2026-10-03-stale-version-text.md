## One version string and no stale capability notes

Text inside NovaOS had fallen behind the OS.  The version now lives in one
place, `kernel/ke/version.h` (`NOVA_VERSION`), and the boot banner,
Settings > About, the Terminal's `ver` and title line and `sysinfo` all
print it, so Settings no longer says "Phase 9.5 desktop" and `ver` no longer
says "Phase 8 desktop".  Roadmap phases are tracked in `docs/roadmap/`, not
in the OS.

- **`sysinfo`** reports the real SMP state and CPU count ("SMP on, 4 CPUs")
  from `g_cpu_count` instead of a fixed "SMP off".
- **App Store.**  VLC and Audacity no longer say they need audio output
  NovaOS lacks (both play and record since the WASAPI work); OBS no longer
  says Direct3D is missing and is marked untested.

## The App Store

The dock's **App Store** (`kernel/apps/store.c`, also `start store` in the
Terminal) is a catalog of free and open-source Windows programs: 7-Zip,
VLC, Firefox, Thunderbird, Notepad++, GIMP, Inkscape, Krita, Audacity,
HandBrake, OBS Studio, LibreOffice, SumatraPDF, KeePassXC, qBittorrent,
PuTTY, WinSCP, Git, Python, WinMerge and ShareX, plus a **Runtimes**
category (.NET Desktop Runtime, Visual C++ Redistributable, OpenJDK,
Mesa 3D), by category, with a note on how far each gets on NovaOS today.

- **64-bit packages where there are any.**  The store fetches each
  project's official 64-bit package: a portable `.zip` or `.7z`, an
  installer, a Windows Installer `.msi`, or, for Firefox and Thunderbird,
  the full installer, which is a 7-Zip self-extracting archive.  Apps
  whose only download is a 32-bit setup program (GIMP, qBittorrent,
  WinSCP, the Visual C++ Redistributable) now install through it too,
  since NovaOS runs 32-bit programs.
- **Get** downloads over HTTPS (following redirects, with the bytes
  received shown while it runs) to `C:\Downloads`, using the same network
  operations as the Terminal's `wget`.  Files may be up to 256 MB.
- **Install** depends on the package: archives are unpacked into
  `C:\Programs\<App>` by the installed 7-Zip's own `7z.exe`, unchanged
  (the store waits for it and reports its result, and asks for 7-Zip
  first if it is missing); `.msi` packages go to NovaOS's Windows
  Installer; installers, 64-bit or 32-bit, run.
  **Run** starts portable programs (PuTTY, SumatraPDF) from Downloads.
- **Open** starts the installed program from its own folder (console
  programs such as Python and Git in a Terminal); archives with a
  versioned top folder are found wherever the program landed.  The
  **Installed** view lists what is there; runtimes show "Installed".
- Tested in QEMU with stand-in packages (the sandbox this was built in
  cannot reach the publishers): 7-Zip installed from its x64 installer;
  a portable zip unpacked into `C:\Programs\Notepad++` and opened; a
  7-Zip self-extracting installer unpacked like Firefox's and opened.
  The apps themselves mostly need more of Windows than NovaOS has (the
  note on each row).
- Programs in a folder of their own under `C:\Programs` now start in
  that folder when opened from the desktop (they used to be looked up
  by name and not found).
- The Terminal gained `copy <source> <destination>`.
- A kernel bug the store shook out: `ksnprintf` looped forever when a
  `%s` argument had to be cut to fit the buffer, freezing the desktop on
  a long error message.

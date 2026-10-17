# NovaOS user guide

NovaOS is an operating system that runs Windows programs as they are,
without Windows.  This guide covers using it: trying it in a virtual
machine, installing it, the desktop and the built-in apps, and installing
programs.  To build NovaOS from source, see [building.md](building.md);
on a Mac, see [macos.md](macos.md).

> **NovaOS is early software.**  It is developed and tested in the QEMU
> virtual machine and has not yet been checked on any real PC
> ([hardware.md](hardware.md)).  Run it in a virtual machine, and do not
> install it on a computer whose disk holds anything you need.
> [compatibility.md](compatibility.md) lists which Windows programs run.

## Getting NovaOS

NovaOS comes as one file, `nova.iso`, which is both a live system and its
own installer.  There are two places to get it:

| Build | Where | What it is |
|---|---|---|
| Newest release | [releases/latest](https://github.com/dean-plude/os/releases/latest) | A tagged version (the first was v0.1.0, 2026-10-04) |
| `latest` | [releases/download/latest/nova.iso](https://github.com/dean-plude/os/releases/download/latest/nova.iso) | Rebuilt from `main` after every push whose CI run passes; newer, less tested |

Each comes with `nova.iso.sha256`.  Check the download with
`sha256sum -c nova.iso.sha256` (on a Mac, `shasum -a 256 -c nova.iso.sha256`).

## Trying it in a virtual machine

NovaOS needs a 64-bit x86 machine with UEFI firmware.  QEMU is the
virtual machine it is tested in.  Install it:

- **Linux** (Ubuntu, Debian): `sudo apt install qemu-system-x86 ovmf`
- **Windows**: inside WSL2, as on Linux.
- **Mac**: `brew install qemu`.  [macos.md](macos.md) has the commands
  for Apple Silicon and Intel Macs, and for UTM.

Then, on Linux, start the ISO with an empty 1 GB disk to install onto:

```bash
truncate -s 1G disk.img
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file=/tmp/OVMF_VARS.fd \
  -drive file=disk.img,format=raw -cdrom nova.iso
```

Useful extra options:

| Option | Effect |
|---|---|
| `-enable-kvm -cpu host` | Runs NovaOS at close to native speed instead of emulating the CPU.  Needs access to `/dev/kvm`. |
| `-device intel-hda -device hda-duplex` | A sound card with speakers and a microphone. |
| `-serial stdio` | Shows NovaOS's log in your terminal. |

QEMU's default network works without any options.  Give the machine at
least 2 GB of memory: drive C: is held in memory while NovaOS runs (see
[Your files](#your-files)).

**Hyper-V**: make a Generation 2 VM with Secure Boot off and start it from
the ISO.  NovaOS has drivers for Hyper-V's keyboard, mouse, virtual
hard disks (so it installs to a VHDX), video (so the resolution can be
changed in Settings) and network adapter (the Default Switch works), and
the host's Shut Down and Restart work ([hyperv.md](hyperv.md)).

**VirtualBox, VMware and others** are untested.  The closest match to
what NovaOS drives is a machine set to "Other 64-bit" with EFI on, a SATA
(AHCI) or NVMe disk, and an Intel PRO/1000 (e1000) network card.

## Installing

Started from the ISO, NovaOS runs live: everything works, but nothing you
do is kept after a restart.  **Install NovaOS** opens by itself, and is
also on the desktop:

1. Choose the disk to install on.  NovaOS installs on SATA (AHCI), NVMe
   and USB disks of at least 256 MB.  **The whole disk is erased.**
2. Choose the file system for drive C:: **NTFS** (the default; keeps file
   permissions) or **FAT32**.
3. When it has finished, remove the ISO (in QEMU, start again without
   `-cdrom nova.iso`) and restart from the disk.

Anything you saved on C: during the live session is copied to the new
installation.  In the Terminal, `install` lists the disks and
`install <disk> [/fat]` installs without the window.

## First start

The first time an installed NovaOS starts, **Welcome to NovaOS** opens
before the desktop.  It asks, one page at a time:

![Welcome to NovaOS asking for the time zone](screenshots/first-start-setup.png)

1. **Your name.**  It is shown on the Start menu and on the desktop, and
   programs get it as the user name.
2. **Your time zone.**  Type a city (Berlin, Tokyo, New York...) or pick
   one from the list.  The clock, Calendar, the Terminal's `date` and
   `time` and Windows programs then show local time, with daylight saving
   time where the zone has it.
3. **Your keyboard layout**: English (US, UK or US Dvorak), German, Swiss
   German, French, Swiss French, Canadian French, Spanish, Italian,
   Portuguese, Brazilian Portuguese, Swedish, Finnish, Norwegian or
   Danish.  The layout changes at once; try it in the box under the list.
   AltGr (right Alt) types the third character on a key, and accent keys
   put their accent on the next letter.
4. **The display resolution.**  It changes at once.

To change these later: Settings > Time & language (time zone, keyboard
layout and regional format), Settings > Display (resolution), or
`start welcome` in the Terminal to go through every page again.  In the
Terminal, `tzutil /s "NAME"` sets a time zone by its Windows name and
`tzutil /l` lists the names.

## The desktop

![The NovaOS desktop](screenshots/desktop.png)

- **The dock** along the bottom holds Start, search, the pinned apps
  (Terminal, File Explorer, Notepad, Settings, Calendar, Firefox and the
  App Store) and a button for every open window.  A dot under an app
  means it is running.  The network status and the clock are on the
  right.  A full-screen program, such as a game, covers the dock; it comes
  back when you switch away or press the Windows key.
- **Start** (the Windows key, or the first dock button) lists pinned apps,
  installed programs, recent apps and documents, and the power menu.
  Start typing to search apps, programs, settings pages, folders and
  files; Enter opens the highlighted result.
- **The workspace.**  On a display of at least 900x640 with the default
  Aurora theme, the desktop also shows a sidebar of apps, a search bar
  (Ctrl+K), your name, a calendar and shortcuts to File Explorer, Notepad
  and Firefox.  Settings > Personalization switches to the Sunset, Ocean
  or Twilight theme, which show plain desktop icons instead.
- **Wallpapers and icons** are files, not part of NovaOS's kernel:
  wallpapers are the PNG pictures in `C:\Windows\Web\Wallpaper` (Beam,
  Nova Night, Horizon, Glass Dawn, Aurora Lights, the three Waves and the
  Aurora city), and icon sets are folders of `.ico` files in
  `C:\Windows\Web\Icons` (NovaOS comes with **Beam**).  In Settings >
  Personalization, **Background** picks a theme or a picture and **Icons**
  picks an icon set or the built-in icons; your choice is kept.
  - To add a wallpaper, save a PNG picture in Downloads, Pictures,
    Documents or Desktop (or a folder in one of them): it is listed under
    **Add a picture**, and **Add** copies it to the wallpaper folder and
    shows it.
  - To add an icon set, save a folder of icons there named as NovaOS's
    are (`terminal.ico`, `file-explorer.ico`, `folder.ico`,
    `folder-documents.ico`, `this-pc.ico`, ...;
    [design/brand/README.md](design/brand/README.md#icon-sets) lists them):
    it is listed under **Add an icon set**.  Icons a set has no file for
    stay as they are.
- **Windows** move by their title bar and resize from any edge.  Drag one
  to the side of the screen to snap it to that half.
- **Right-click** the desktop, an icon, the dock or a title bar for a
  menu.

![The Start menu](screenshots/start-menu.png)

Keyboard shortcuts:

| Keys | Does |
|---|---|
| Windows | Open or close Start |
| Windows+S, or Ctrl+K on the Aurora desktop | Search |
| Windows+E | Open File Explorer |
| Windows+D | Show the desktop (again to bring the windows back) |
| Windows+Left / Right | Snap the window to the left or right half (again to restore it) |
| Windows+Up | Maximize the window |
| Windows+Down | Restore a maximized or snapped window, or minimize it |
| Alt+Tab | Switch windows (Shift+Alt+Tab goes back, Esc cancels) |
| Ctrl+Shift+Esc | Open Task Manager |

## The built-in apps

| App | What it does |
|---|---|
| **File Explorer** | Shows This PC (every drive and its free space) and the folders Documents, Downloads, Pictures, Personal, Projects and Windows.  Double-click a program (`.exe`) to run it, a Windows Installer package (`.msi`) to install it, a picture (`.png`, `.ico`, `.cur`) to see it in Photos, a shortcut (`.lnk`) to open its target, and anything else to read it in Notepad.  Ctrl+C, Ctrl+X and Ctrl+V copy and move files, also to and from Windows programs; Delete deletes. |
| **Terminal** | The command line (below). |
| **Settings** | System, Display (resolution, scale, several monitors), Sound (output and input devices and their volumes), Personalization (theme, wallpaper picture and icon set, and adding wallpapers and icon sets), Storage, Network, Time & language, and About. |
| **Firefox** | Mozilla's Firefox 157 for Windows, unmodified and built into NovaOS as its web browser. |
| **App Store** | Downloads and installs open-source Windows programs (see [Installing programs](#installing-programs)), and updates NovaOS. |
| **Task Manager** | Ctrl+Shift+Esc, or right-click Start.  **Processes** lists the open apps and the background processes with their CPU, memory and status; select one (click it, or type its name) and press **End task** or Delete to end it.  **Performance** graphs the last minute of each logical processor, memory, disk and Ethernet.  **Details** shows every process with its PID, user, CPU time, threads, handles and priority, and **Services** the Windows services and their state.  Ctrl+Tab moves between the views; click a column heading to sort by it. |
| **Device Manager** | Type "Device Manager" in Start.  Lists the PC's devices by type (or by connection, View menu), as Windows' Device Manager does; select one to see its status and problem code, driver (provider, version, date), hardware and compatible IDs and resources (memory and I/O ranges, IRQ).  F5 scans for hardware changes; it also refreshes when a device is plugged in or removed. |
| **Event Viewer** | Type "Event Viewer" in Start.  Shows the Application, System and Security logs as Windows' Event Viewer does: each event's level, date, source, event ID and task category, and below the list its message (General) and its XML (Details).  The Action menu filters by level, clears a log and saves its events as `.evtx` or `.xml`.  NovaOS logs each start, services starting and stopping, drivers that fail to load and programs that crash; programs add their own events. |
| **Calculator** | Type "Calculator" in Start, or `calc` in the Terminal.  Windows Calculator's own engine, so its answers match Windows': **Standard** works each operator at once, **Scientific** keeps precedence and adds powers, roots, logarithms, trigonometry (DEG, RAD, GRAD) and factorials, and **Programmer** shows the value in hexadecimal, decimal, octal and binary with the bitwise operators and word sizes.  Alt+1, Alt+2 and Alt+4 change calculator; type the expression on the keyboard (Esc clears, Ctrl+C copies the result, Ctrl+V pastes); MS, MR, M+ and M− keep a number in memory. |
| **Notepad**, **Photos**, **Calendar** | A text editor, a picture viewer (PNG and icons) and a calendar. |
| **NetSurf** | A small web browser; type `netsurf [url]` in the Terminal. |
| **Camera** | Shows a USB camera's picture; type `camera` in the Terminal or find it with Windows+S.  The camera must stream on a bulk endpoint for now: most built-in webcams do not yet work (see [drivers.md](drivers.md#cameras-usb-video-class)). |

### The Terminal

![The Terminal running sysinfo and devices](screenshots/terminal.png)

The Terminal has its own commands; `help` lists them.  The most useful:

| Command | Does |
|---|---|
| `dir`, `cd`, `type`, `copy`, `del`, `mkdir` | Work with files |
| `start <app> [file]` | Open a built-in app (`notepad`, `explorer`, `settings`, `calendar`, `browser`, `store`, `welcome`) |
| `store install <name>` | Install a program from the App Store |
| `winget search`, `install`, `list`, `upgrade`, `uninstall` | The Windows Package Manager ([below](#winget)) |
| `ipconfig`, `ping`, `nslookup`, `curl`, `wget` | Network |
| `tasklist`, `taskkill /PID n` or `/IM name.exe` | List and stop programs |
| `devices` | PCI devices and the driver each one has |
| `sysinfo`, `ver`, `dmesg`, `crashes` | About the system, its log and crash reports |
| `vol`, `sync` | Where drive C: is saved; save it now |
| `install`, `update` | Install NovaOS on a disk; update it |

Any other name runs that program, so Windows command-line programs work
as they do on Windows.  NovaOS also includes its own versions of some
Windows tools, such as `cmd` (runs NovaOS's `cmd.exe`, for batch files),
`find`, `findstr`, `sort`, `more`, `less`, `sc`, `pnputil`,
`tzutil`, `msiexec` and `shutdown`.  Up and Down recall earlier commands.

### DOS programs

![bootRogue, a DOS game, in the Terminal](screenshots/dos-bootrogue.png)

16-bit DOS programs (`.COM` and `.EXE`) run as Windows ran them through
NTVDM: type the name in the Terminal or `cmd`, or double-click one in File
Explorer, and it runs in the console window on MS-DOS 4.00 with drive C:
and the current folder as you have them (long names show as 8.3 names,
such as `MYDOSP~1`).  When it ends the window is the Terminal's again and
`%ERRORLEVEL%` is its exit code.  `ntvdm` on its own boots to an `A:\>`
prompt with all of MS-DOS 4.00's commands (F12 leaves).  To try a game,
type `C:\Tests\Dos\ROGUE`: arrow keys move, Esc quits.  Text-mode programs
only for now: no graphics, sound or mouse.  Details in [dos.md](dos.md).

## Installing programs

The **App Store** in the dock lists programs by category.  For each one,
**Get** downloads it to `C:\Downloads`, **Install** installs it (by
running its setup program or Windows Installer package, or by unpacking
its archive with 7-Zip), and **Open** starts it.  Installed programs also
appear in Start.  In the Terminal, `store install <name>` does the same.
A download that fails on the way (no data for 30 seconds, a dropped
connection, a busy server) is tried again from the start, up to three
times, before the row says **Failed** with the reason and the server's
name; each failed try is also in the serial log (`[STORE] ...`).
Downloads can be up to 1 GB, held in memory until they are saved.
A **Verified** badge next to a program's name means the nightly tests
installed that same download and it passed last night (with its rating
from [compatibility.md](compatibility.md)); **Failed last nightly**
means it did not.

Some programs need a runtime from the App Store's **Runtimes** category
first; the program's note in the App Store says which:

| Program uses | Install first |
|---|---|
| OpenGL | Nothing: Mesa's OpenGL (llvmpipe, on the CPU) is built in |
| Direct3D 8 to 11 | **Mesa 3D** (for its Vulkan) and **DXVK** |
| Direct3D 12 | **Mesa 3D**, **DXVK** and **VKD3D-Proton** |
| A program packed as a `.zip` or `.7z` | **7-Zip** (Utilities), which the App Store uses to unpack it |

A graphical program started from Start, the App Store, File Explorer or a
shortcut opens in its own window.  Only a console (command-line) program
gets a Terminal window, as its console.

Programs from anywhere else install as on Windows: download the installer,
`.msi` or archive, then run the installer, or unpack the archive (with
7-Zip from the App Store) and run the program.  NovaOS does not yet
implement everything Windows programs use, so many programs still fail.
[compatibility.md](compatibility.md) lists the ones that have been
checked.

The App Store's list of programs is a JSON file that can be updated
without updating NovaOS (**Refresh** in the App Store, or
`store refresh`).  [app-store-catalog.md](app-store-catalog.md) explains
how.

### winget

`winget`, the Windows Package Manager's command, works in the Terminal and
in `cmd` with Windows' commands and options:

| Command | Does |
|---|---|
| `winget search <words>` | Find packages (by name, identifier or moniker) |
| `winget show <package>` | A package's details and the installer NovaOS would use |
| `winget install <package>` | Download, check and install it (`-v` a version, `-s` a source, `-i` with the installer's own questions) |
| `winget install -m <manifest>` | Install from a manifest file or folder on disk |
| `winget list` | Installed programs, with newer versions where a source has one |
| `winget upgrade` | What can be upgraded; `winget upgrade --all` or `winget upgrade <package>` upgrades |
| `winget uninstall <package>` | Remove a program |
| `winget source list`, `add`, `remove`, `update`, `reset --force` | Package sources |
| `winget hash <file>` | A file's SHA-256, as manifests give it |

It has three kinds of source:

- **novaos**: the App Store's catalog, installed exactly as the App Store
  does (so `winget install 7-Zip` and **Get**, **Install** in the App Store
  are the same).  Programs NovaOS has been tested with come from here
  first, under their winget identifiers where they have one
  (`winget install 7zip.7zip`).
- **winget**: the community repository Microsoft's winget uses,
  [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs).
  NovaOS reads its manifests from GitHub, so it finds a package by its
  exact identifier, as written there (`winget install Mozilla.Firefox`,
  `winget search Gyan.FFmpeg`); searching it by words needs Microsoft's
  index, which NovaOS does not read yet.  Its programs have not
  necessarily been tested on NovaOS.
- **REST sources** your organization runs (`winget source add <name>
  <url>`), as on Windows.

The download's SHA-256 must match the manifest's.  Setup programs run
silently with the switches the manifest gives (or winget's usual ones for
Inno Setup, NSIS, WiX Burn and Windows Installer); a portable program is
copied to `C:\Programs\<command>\<command>.exe`, so typing its command
runs it; `.zip` packages need **7-Zip** (`winget install 7zip.7zip`).
MSIX and Microsoft Store packages cannot be installed: they need Windows'
package deployment, which NovaOS does not have.  `winget list` shows what
winget and the App Store installed and every program with an entry in
Windows' installed programs list.

### Drivers

A Windows driver package (an `.inf` file with its `.sys` and `.cat`
files) installs as on Windows:

- run its setup `.exe` or `.msi`, or
- unpack it and run `pnputil /add-driver <folder>\*.inf /install` in the
  Terminal.

| Command | Does |
|---|---|
| `pnputil /enum-drivers` | List the installed driver packages |
| `pnputil /delete-driver oemN.inf` | Remove one |
| `pnputil /enum-devices` | List devices; problem code 28 means no driver (Device Manager shows the same) |
| `pnputil /disable-device /deviceid <ID>`, `/enable-device` | Turn a device off or on |
| `pnputil /scan-devices /search-online` | Find drivers for devices that have none on the Microsoft Update Catalog |

`/install` starts the driver on each device it is for that NovaOS does
not already drive itself.  A device NovaOS has its own driver for keeps
it, and `pnputil` says no device took the package.  A kernel-mode driver
that is not tied to a device runs as a service: `sc create NAME type=
kernel binPath= ...`, then `sc start NAME`.  Either kind loads only if
it is signed the way Windows 11 requires: by Microsoft, or with a test
signature while test signing is on
([drivers.md](drivers.md#driver-signing)).

`/search-online` looks up each device's hardware IDs on the Microsoft
Update Catalog, as Windows Update does, downloads the matching package,
checks it against the Catalog's SHA-256 and adds it to the driver store
([drivers.md](drivers.md#downloading-drivers-by-hardware-id)).  Nothing is
downloaded unless you run it.

Release builds also carry the device firmware some of NovaOS's own
drivers load ([drivers.md](drivers.md)).

## Your files

Drive C: holds your files and the programs you install.  While NovaOS
runs, C: is held in memory.  On an installed NovaOS it is also kept on
the disk: NovaOS saves changes once C: has been quiet for a second, and
again before a restart or shutdown.  In the Terminal, `vol` shows where
C: is saved and `sync` saves it at once.

Running live from the ISO, NovaOS keeps nothing unless it finds somewhere
to save C:: an empty disk (which it formats as FAT32 and labels
`NOVADATA`) or a volume already labelled `NOVADATA`.  So to keep files
without installing, give QEMU a second empty disk.
[building.md](building.md#where-your-files-are-kept) has the details.

Other NTFS and FAT32 disks and USB sticks appear as D:, E: and so on, in
File Explorer and to programs.

**Disk Management** (search the Start menu for it) shows the volumes
and, below, each disk with its partitions and unallocated space.
Right-click a disk or a partition to:

- **Initialize Disk** a new, empty disk (GPT, or MBR for older PCs);
- make a **New Simple Volume** in unallocated space: its size, drive
  letter, file system (NTFS or FAT32) and label;
- **Format** a volume as NTFS or FAT32 (everything on it is erased: you
  are asked first);
- **Change Drive Letter and Paths**: a new letter, or none.  NovaOS
  remembers it for that volume.

The volumes NovaOS itself uses (where C: is saved, the boot files) are
shown but cannot be formatted or given another letter.

![File Explorer on C:\Windows](screenshots/file-explorer.png)

## Network and sound

**Wired networking** starts by itself: NovaOS gets an address from the
network's DHCP server.  Settings > Network, or `ipconfig` in the
Terminal, shows it.  To set the address by hand, choose **Edit** on the
Ethernet card in Settings > Network, pick **Manual** and enter the IP
address, subnet mask, gateway and DNS servers, then **Save**.  With
**Automatic (DHCP)** you can still enter DNS servers to use instead of
DHCP's (leave them blank to use DHCP's).  The Terminal does the same the
Windows way:

```text
netsh interface ipv4 set address name=Ethernet static 192.168.1.50 255.255.255.0 192.168.1.1
netsh interface ipv4 set dnsservers name=Ethernet static 1.1.1.1
netsh interface ipv4 add dnsservers name=Ethernet 8.8.8.8 index=2
netsh interface ipv4 set address name=Ethernet source=dhcp
netsh interface ipv4 show config
```

![Settings > Network](screenshots/settings-network.png)

Changes take effect at once and are kept across restarts, in the registry
under `HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters`.

**Wi-Fi** cannot connect yet.  With a Wi-Fi card whose Windows driver
NovaOS can open, the Terminal lists the card and the networks in range,
as on Windows:

```text
netsh wlan show interfaces
netsh wlan show networks mode=bssid
```

**Sound** plays through the newest output device.  Settings > Sound picks
another device and sets each device's volume; the choices are kept across
restarts.  On the reference laptop (ThinkPad T14 Gen 4), the built-in
microphones sit behind Intel's audio DSP and appear as "Microphone Array
(DSP)" when the DSP firmware is included in the build
([hardware.md](hardware.md#the-digital-microphones-behind-the-audio-dsp)).

## Updating NovaOS

An installed NovaOS updates itself.  The App Store's **Updates** page (or
`update` in the Terminal) checks for a newer version; **Update** downloads
and checks it, and **Restart** starts it.  If the new version fails to
start, the next start goes back to the old one and the Updates page says
so.  NovaOS running live from the ISO or a USB stick does not update;
download a newer ISO instead.  [updates.md](updates.md) has the details,
including how to follow the `latest` builds instead of releases.

## Sleep, restart and shut down

Start's power menu has Restart and Shut down, and Sleep on machines that
support it.  NovaOS saves drive C: before it restarts or shuts down.

## When something goes wrong

- **A program fails**: look it up in [compatibility.md](compatibility.md).
  `tasklist` lists running programs; `taskkill /PID n` stops one, and
  `taskkill /IM name.exe` stops every copy of a program.
- **A program crashes**: NovaOS writes a crash report (the exception,
  where it happened, the stack and the loaded modules) to
  `C:\NovaOS\Crashes`.  `crashes` lists them and `crashes last` shows the
  newest.  Event Viewer's Application log lists every crash too
  (Application Error, event 1000), and its System log lists services
  that stopped unexpectedly and drivers that failed to load.
- **The log**: NovaOS writes what it does to the serial port (QEMU's
  `-serial stdio` shows it in your terminal).  Started from a USB stick,
  it also writes it to `EFI\NOVA\bootlog.txt` on the stick.  `dmesg` in
  the Terminal shows it too.
- **Reporting a problem**: open an issue on
  [GitHub](https://github.com/dean-plude/os/issues) with what you did,
  what happened, and the log or crash report.

## Real PCs

NovaOS should start on a UEFI PC with Secure Boot turned off, from a USB
stick written from `nova.iso` (the [README](../README.md) says how).  Its
reference machine is the Lenovo ThinkPad T14 Gen 4 (Intel), but **no real
PC has been checked yet**.  [hardware.md](hardware.md) lists which of the
T14's devices NovaOS has drivers for and what is missing, and
[install-and-power.md](install-and-power.md) the checks still to be done
on it.

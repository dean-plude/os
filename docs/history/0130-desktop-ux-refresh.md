## Desktop UX refresh
- **Start menu** (`wm/desktop.c`): live search as you type (Win key, then
  type) over the built-in apps, the programs in `C:\Programs`, Settings
  pages ("display", "wallpaper", "wifi"...) and every file and folder on C:.
  Up/Down pick a result, Enter opens it, Esc clears the query and then closes
  the menu.  The home view shows pinned apps, the installed programs (click
  to run in a Terminal), recently used apps, files and folders, and a footer
  with Files, Settings and a power menu (Restart, Shut down).
- **Window management** (`wm/wm.c`): drag a title bar to the left or right
  edge to snap to half the screen, or to the top to maximize (a translucent
  preview shows where it will land); dragging a tiled window restores its
  size.  Resize windows from any edge or corner.  Keyboard: **Alt+Tab**
  (Shift+Alt+Tab backwards) with a switcher, **Win+Left/Right/Up/Down**
  (snap, maximize, restore/minimize), **Win+D** (show desktop and back),
  **Win+E** (File Explorer), **Win+S** (search).
- **Right-click menus** on the desktop (Terminal, File Explorer, show
  desktop, next wallpaper, Personalize, Display settings), desktop icons
  (Open, Open in Terminal), title bars (maximize/restore, snap, minimize,
  close) and dock items (open or new window, close).
- **Dock and tray**: the Start menu, dock and tray are neutral dark
  frosted glass (`GdiBackdrop` blurs whatever is behind them, then tints
  it), so they sit well on any wallpaper.  The dock holds only launchers
  and windows, on a fixed grid (40 px targets, 28 px app tiles, 22 px
  glyphs); every window without a pinned app gets its own button with a
  running indicator.  The tray, a separate block at the right, shows the
  network status (tooltip: address; click: Settings > Network) and the
  clock (tooltip: full date; click: Calendar).
- **Themes**: three wallpapers (Sunset, Ocean, Twilight), chosen in the new
  **Settings > Personalization** page.
- **One icon style**: apps are rounded-square tiles with a white line glyph;
  folders (special ones show their purpose: Documents, Downloads, Pictures,
  Projects), documents and This PC are flat two-tone shapes; sidebars,
  toolbars and the tray use single-colour line glyphs; every line is 8% of
  the icon size thick.  The Start button is NovaOS's star.
- **Windows**: the title bar is a shade darker than toolbars, with a
  hairline under it, and every window has a faint light edge.
- **File Explorer**: a command bar with back/up, a clickable breadcrumb
  path (This PC > Local Disk (C:) > Documents; long paths collapse from the
  left) and "+ Folder" / "+ File" buttons; the sidebar has line glyphs per
  place (now including Downloads) and is divided from the list.
- **Desktop**: This PC, Documents, Downloads, Pictures and Projects open
  File Explorer there; NetSurf has an icon.  The selection is a soft
  translucent fill, and labels have a blurred drop shadow so they stay
  readable on bright wallpaper.
- **Windows icons (.ico)** (`gdi/icon.c`, `gdi/png.c`): the kernel reads
  .ico and .cur files, whose images may be BMP-style DIBs (1, 4, 8, 16, 24
  or 32 bits per pixel with the AND mask) or PNG (as 256x256 Vista-style
  icons are), and the icons in a program's resources (RT_GROUP_ICON /
  RT_ICON in any .exe or .dll).  Drawing picks the image that best fits the
  size at device resolution and scales it with alpha blending.  PNG is
  decoded by a small built-in inflate + PNG decoder (every colour type, bit
  depth and interlacing; it matches libpng on the PngSuite images).
  A program shows its own icon (a `NAME.ico` next to `NAME.exe` wins over
  the one in its resources) in the Start menu, search, the title bar, the
  dock, Alt+Tab and File Explorer; Explorer also shows .ico files as
  themselves and PNG thumbnails, with file types ("Icon", "Application",
  "PNG image").  Programs get icons at build time from `programs/NAME.rc`
  (compiled by `llvm-rc`): `winhello.exe` has one drawn by
  `tools/make_icons.py`, and `netsurf.exe` carries NetSurf's own.
- **Photos** opens .ico, .cur and .png files: the picture, and for icons
  every image in the file (size and colour depth) in a strip, the selected
  one enlarged pixel for pixel; Left/Right step through the folder, and on
  its own it shows a gallery of `C:\Pictures` (which now holds a sample
  icon and picture).  .exe files opened from Explorer run.
- Not yet: `LoadIcon`/`DrawIcon` and `WM_SETICON` for programs (a window
  shows its program's first icon), and animated cursors.  *(Phase 12's
  user32 has icons from `.ico` files and PE resources and `WM_SETICON`;
  animated cursors came with "Program pointers and animated cursors"
  below.)*

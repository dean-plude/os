## Aurora desktop workspace

The default desktop now follows the Aurora reference's blue and peach
sunrise palette, translucent floating panels, left launcher, top search
and centered dock. The generated city wallpaper is bundled as
`C:\Pictures\Aurora-Sunrise.png`; it is decoded once at shell startup
outside the file-system lock, then reused by painting, theme previews and
other monitors. A deterministic procedural skyline supplies a fallback
if the asset is missing or decoding fails. Sample assets participate in
the build's dependencies so wallpaper changes rebuild the embedded files.

At logical resolutions of at least 900x640, the sidebar groups Home, Apps,
Create and Explore above pinned Notes, Calendar, Photos and Terminal,
followed by Settings and (on live installation media) Install NovaOS.
These launch actual NovaOS applications; user-created desktop files remain
compact double-click icons beside the sidebar. Search uses Ctrl+K or the
existing Win+S shortcut. The header displays the configured user's name,
initials and actual local date. A live calendar opens Calendar, and a
second glass card greets the user and opens Explorer, Notepad and NetSurf.
The calendar handles leap years and six-week months; it refreshes with the
shell's minute timer and time-zone changes. App windows cover these
background cards normally. Hovering a dock item raises it visually while
its input rectangle remains stable.

Explorer opts into the Aurora acrylic backdrop and opens wider when room
permits. Its native preview pane shows the selected file's thumbnail,
name, type, size and decoded PNG dimensions (or a folder's item count).
Open uses the existing file/folder handler; Copy path writes its real path
to the system clipboard. The pane collapses below a 902x400 client size,
leaving the existing list and scroll bars their full width. Selection,
keyboard navigation, drive handling and file operations retain their
existing implementations.
Smaller logical displays retain the icon grid. The Sunset, Ocean and
Twilight themes keep their prior indices, appearance and interactions.
Dock task buttons, running indicators, menus, full-screen behavior and
keyboard shortcuts retain their existing implementations. Weather, AI
chat and resource gauges from the concept image are not implemented.

The wallpaper was created with the built-in image-generation tool using
the supplied screenshot as the edit reference: remove all interface
overlays and reconstruct the cinematic city, river, bridges, mountain,
sunrise and subtle crescent planet, with no text or logos. The final asset
is `userland/samples/Aurora-Sunrise.png`.

Validation: the shell translation unit compiles with the kernel's
freestanding GCC flags; Explorer and the window manager also compile. A temporary host harness decoded the bundled PNG
with NovaOS's decoder and rendered the workspace with its actual GDI,
fonts and icons at 1672x941, 900x640 and 800x600. It also checked hotspot
counts and that all 24 user-file cells clear the cards and dock at the
workspace's minimum size. Preview checks cover the size thresholds,
Open's file dispatch, folder navigation, Copy path's clipboard payload,
empty selection and clicks outside the pane. `docs/screenshots/aurora-preview.png` is this
host-rendered preview with a fixed date and Explorer displaying the real bundled PNG through
file-system stubs; it is not an OS boot screenshot. Manifest, self-test discovery, generated-document,
conflict-marker and whitespace checks pass. A full kernel build and QEMU
boot were not available on this host (no clang, NASM or QEMU); they remain
required before merging.

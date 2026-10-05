# NovaOS desktop implementation

`novaos-2030-reference.html` preserves the user-supplied prototype as a
design reference. The operating system renders its desktop through the
native window manager and GDI; it does not load the prototype's remote
scripts, fonts, images or API calls.

| Prototype feature | Native implementation |
|---|---|
| City wallpaper and translucent surfaces | Bundled PNG, native blur, dark blue tint, rounded edges and shadows |
| Home / Apps / Create / Explore | Explorer / Start launcher / Notepad / NetSurf |
| Pinned app group | Notes, Calendar, Photos, Terminal, Settings; installer on live media |
| Global search | Actual app, file and settings search, Ctrl+K and Win+S |
| Clock, date and profile | Local system time and configured user name |
| Dock hover and running indicators | Raised hover appearance, stable hit targets, real window activation and task indicators |
| Explorer preview | File thumbnail, real size/type, PNG dimensions, folder item count, Open and Copy path |
| Calendar | Actual local month and today, leap-year handling, opens native Calendar |
| Window dragging, resizing and controls | Existing native window manager |
| AI chat / analysis / automation | Requires an implemented assistant service and account configuration; not supplied by the native shell |
| Weather, CPU/GPU gauges, music and update notices | Prototype demo values and simulated actions; not displayed as live OS results |
| Spaces, cloud drives, mail and canvas | Separate features still to implement |

The native code keeps the original themes and uses a compact icon-grid
layout below 900x640 logical pixels. Explorer hides its preview below a
902x400 client size. No mock conversations, random utilization values or
fictional update notifications are injected into the OS.

The rendering preview in `../screenshots/aurora-preview.png` uses the
native GDI, font, icon, window-frame and Explorer painters on a host
buffer, with file-system stubs and the actual bundled wallpaper file.
Local compile, manifest, discovery, documentation and layout/preview
interaction checks pass. A complete kernel build, QEMU boot and the
existing graphics self-tests remain necessary before merging.

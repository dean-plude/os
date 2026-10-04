## The Windows Runtime's UISettings, for Qt's Windows platform plugin

GOG GALAXY's client (`GalaxyClient.exe`, 64-bit Qt 6) loaded and then
ended at once: Qt's Windows platform plugin (`qwindows.dll`) builds its
palette from the Windows Runtime class
`Windows.UI.ViewManagement.UISettings`, which NovaOS did not have, and
C++/WinRT turned the failed activation into an exception nobody caught.
ole32 now has the class, and the client goes on: setup's first-run pass
installs GOG GALAXY's service and exits normally, and the client opens its
own window (its warning that drive C: is not NTFS).  Past that it stops
because its service, `GalaxyClientService.exe`, ends at start-up
("abnormal program termination"), and the client then faults
([compatibility.md](../compatibility.md)).

- **`UISettings`** (`userland/ole32/uisettings.c`): activates through its
  factory's `ActivateInstance` and through `RoActivateInstance`, which now
  works for every class NovaOS has, with `IUISettings` to `IUISettings6`.
  `GetColorValue` gives what Windows does for the app mode, a white
  background with black text in the light mode and the reverse in the
  dark one
  (`AppsUseLightTheme` under `HKCU\...\Themes\Personalize`; without it,
  light, as user32 draws), and the accent and its six shades from the
  `AccentPalette` Windows keeps under `...\Explorer\Accent` (without it,
  Windows 10's default blue, `0078D7`, which dwmapi reports too).
  `UIElementColor` gives the system colours; the cursor and scroll bar
  sizes, caret, double-click and hover times come from user32;
  `TextScaleFactor`, `AdvancedEffectsEnabled` (`EnableTransparency`, off
  without it: NovaOS composes no transparency) and `AutoHideScrollBars`
  from their registry values.
- **Its events**: `ColorValuesChanged`, `TextScaleFactorChanged`,
  `AdvancedEffectsEnabledChanged` and `AutoHideScrollBarsChanged` are
  raised, from a thread of the process as on Windows, when a value they
  report changes: a thread started with the first handler watches the keys
  with `RegNotifyChangeKeyValue`.  A handler is released when it is removed
  or when its `UISettings` goes.
- **`UIViewSettings`**: `IUIViewSettingsInterop::GetForWindow` gives one
  whose `UserInteractionMode` is `Mouse` (NovaOS has no tablet mode);
  `GetForCurrentView`, which needs a `CoreWindow`, fails as it does for a
  Win32 thread on Windows.
- **The next class a program needs**: a runtime class NovaOS lacks is
  still `REGDB_E_CLASSNOTREG`, and its name is now written to the kernel's
  log (`ole32: no Windows Runtime class ...`).
- **`BCryptEnumContextFunctions`**: Galaxy's `CrashReporter.exe` lists
  Schannel's cipher suites (the local `SSL` context's
  `NCRYPT_SCHANNEL_INTERFACE`) and stopped on the missing function; it
  gets Windows 11's list, every one of which secur32's Schannel (Mbed TLS)
  negotiates, and now sends its report and exits normally.
- **Tests**: `qtthemetest` (core suite, 51 checks, 64- and 32-bit) checks
  the colours in both modes and for a new accent, the events being raised
  and not after removal, `UIViewSettings`, an unknown class and the cipher
  suites.

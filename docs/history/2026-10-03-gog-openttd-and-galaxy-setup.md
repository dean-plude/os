## GOG: OpenTTD installs and plays, GOG GALAXY's setup runs

Dean asked for games.  GOG's own copies need an account to download, so
the free game GOG offers, OpenTTD, comes from its own Windows installer
(NSIS) with OpenGFX, and GOG GALAXY from GOG's public offline installer
(Inno Setup 6, 342 MB).

Setup programs need to run as administrator.  The desktop user's token
is now the limited half of an administrator's (Administrators deny-only,
`TokenElevationType` limited) with a full, elevated linked token
(`TokenLinkedToken`); `ShellExecuteEx`'s `runas` verb, a program whose
manifest asks for `requireAdministrator` or `highestAvailable`, and
`CreateProcessAsUser` with the linked token start a program elevated
(`NtSetInformationProcess(ProcessAccessToken)`), and `IsUserAnAdmin`
answers from the token.  Drive C: takes files up to 2 GB (was 256 MB) and
grows a file's buffer by a quarter past 64 MB.

For Inno Setup's wizard: `riched20.dll` and `msftedit.dll` (Rich Edit
2.0 and 4.1 on the Edit control, taking RTF by `EM_STREAMIN` and the rest,
for its licence page), code pages 1252, 28591 and 20127 in
`MultiByteToWideChar` and `WideCharToMultiByte` (RTF's `\'e9` escapes),
oleaut32's `VarAdd` ... `VarCmp` (Delphi's variants), `AddFontResource` of
a bare file name finding the font in the Fonts folder, task dialogs with
custom buttons (Inno's Retry/Ignore/Cancel prompt showed only OK), the
shortcut object's `IPropertyStore` (its AppUserModelID), and
`CryptProtectMemory`/`RtlEncryptMemory` for the Visual C++
Redistributable's installer.

OpenTTD 15.3 installs silently and reaches its main menu: the new app
corpus test `910-gog-openttd`.  GOG GALAXY's setup now copies its files
and makes its shortcuts; starting the client fails on `mfc140u.dll`,
which comes with the Visual C++ Redistributable, whose installer (WiX
Burn) needs MSXML first.  New self-test `setuptest` (x64 and x86) covers
all of the above.

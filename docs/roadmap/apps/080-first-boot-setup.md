- First-boot setup: ~~the user's name and the display resolution~~ Done
  (Phase 22.1): **Welcome to NovaOS** opens the first time an installed
  NovaOS starts, and `start welcome` opens it anywhere.  Still to add:
  a time zone page (NovaOS keeps UTC; `GetTimeZoneInformation` and the
  clock need the chosen zone and its daylight-saving rules) and a
  keyboard layout page (the kernel's key map and user32's
  `GetKeyboardLayout` are US English only).

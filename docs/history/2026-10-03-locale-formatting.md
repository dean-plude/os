## Locale formatting and the user locale

Until now `GetDateFormat`, `GetTimeFormat`, `GetNumberFormat` and
`GetCurrencyFormat` formatted the English way whatever locale a program
asked for, and the user's locale was always `en-US`.

- **Formatting in the locale asked for** (`userland/kernel32/nlsformat.c`):
  `GetDateFormat`, `GetTimeFormat`, `GetNumberFormat` and
  `GetCurrencyFormat`, A, W and Ex (the A number functions are new), take
  their pictures, names, separators, grouping and orders from
  `GetLocaleInfo`, which answers every locale other than English from ICU
  (PR #45).  `de-DE` gives `02.10.2026`, `Freitag, 2. Oktober 2026`,
  `14:05:09`, `1.234.567,89` and `1.234.567,89 €`; `ja-JP` gives
  `2026/10/02`, `2026年10月2日`, `9:05:09`, `1,234,567.89` and `¥1,234,568`,
  as Windows does.  The picture rules are Windows': `d`…`dddd`, `M`…`MMMM`
  (the genitive month when the picture has a day number), `y`, `yy`,
  `yyyy`, `g`, `h`/`H`, `m`, `s`, `t`/`tt` and `'quoted'` text;
  `DATE_LONGDATE`, `DATE_YEARMONTH`, `DATE_MONTHDAY`, `TIME_NOSECONDS`,
  `TIME_NOMINUTESORSECONDS`, `TIME_NOTIMEMARKER` and
  `TIME_FORCE24HOURFORMAT`; `NUMBERFMT` and `CURRENCYFMT` with all five
  negative-number and sixteen negative-currency orders, Indian-style
  grouping, rounding half away from zero, and Windows' errors (a value
  that is not a number, flags beside a format, 30 February, a short
  buffer).
- **Closer to Windows' locale data**: Japanese and Chinese long dates have
  no weekday (`yyyy年M月d日`, where ICU's full date ends in one), and the
  yen sign is Windows' narrow `¥` rather than ICU's full-width one.
- **The user locale** is `LocaleName` under
  `HKCU\Control Panel\International`, read once per process by
  `GetUserDefaultLocaleName`, `GetUserDefaultLCID`, `GetUserDefaultLangID`,
  `GetThreadLocale`, `LOCALE_USER_DEFAULT` and a `NULL` locale name.  The
  system locale and the UI language stay `en-US`.  The registry is saved
  to drive C:, so the choice lasts across restarts.
- **`intl.exe`** (System32) shows the user's format (`intl`), lists the
  locales (`intl /list`) and sets one (`intl de-DE`; a neutral name such
  as `ja` becomes `ja-JP`), writing `LocaleName`, `Locale` and the classic
  values beside them (`sShortDate`, `sDecimal`, `iCurrency`…) for programs
  that read the registry themselves.
- **Settings > Time & language** shows the regional format and offers
  fifteen common ones; a click runs `intl.exe`.
- **Tests**: `nlstest` (core suite, 64- and 32-bit) checks the four
  functions in German, Japanese and English against Windows' output;
  `nlstest user` sets `de-DE`, `ja` and `en-US` with `intl.exe` and checks
  that new processes follow; `nlstest set ja-JP` before the suite's
  restart and `nlstest after-restart ja-JP` after it check the choice
  lasts.
- Not yet: user overrides (a changed `sShortDate` alone is not read back),
  `GetDurationFormat`, alternative calendars (`DATE_USE_ALT_CALENDAR`,
  the Japanese era calendar), and native digits in the output.

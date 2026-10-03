## Calendars, durations and the user's overrides

The locale work left three things open: a changed `sShortDate` (or any
other regional setting) was not read back, `GetDurationFormat` did not
exist, and every date was Gregorian.

- **The user's overrides** (`userland/kernel32/locale.c`): the classic
  values under `HKCU\Control Panel\International` (`sShortDate`,
  `sDecimal`, `sTimeFormat`, `iCalendarType` and the rest of Windows' list)
  are what `GetLocaleInfo`, `GetLocaleInfoEx` and the formatting functions
  answer for the user's locale, unless the caller passes
  `LOCALE_NOUSEROVERRIDE`.  Other locales keep their own values.
  `SetLocaleInfoA`/`W` (new) change one, and set what Windows derives from
  it: a short date sets `sDate` and `iDate`, a time format `sTime`,
  `iTime`, `iTLZero` and `iTimePrefix`, and a new `sDate` or `sTime` is put
  into the format.  A process reads the values once; new processes see
  the change.  `intl NAME` resets them to the chosen locale's own.
- **`GetDurationFormat` and `GetDurationFormatEx`**
  (`userland/kernel32/nlsformat.c`): a duration in 100 ns ticks, or a
  `SYSTEMTIME`'s hours to milliseconds, in a picture of `d`, `h`/`H`, `m`,
  `s` and up to nine `f`; the largest unit in the picture takes what does
  not fit the next one (`h:mm` of a day and a half is `36:00`).  Without a
  picture, the locale's `LOCALE_SDURATION`.
- **Calendars** (`userland/kernel32/calendar.c`): Gregorian and its US
  English, Middle East French and Arabic variants, the Japanese era
  calendar, Taiwan, the Korean Tangun era, Hijri, Thai Buddhist, Hebrew,
  Persian and Um Al Qura (`CAL_GREGORIAN` ... `CAL_UMALQURA`).  A locale's
  calendars are ICU's list of those commonly used where it is, mapped to
  `CAL_*` ids as .NET does, so `th-TH` writes Buddhist years (`2/10/2569`),
  `fa-IR` Persian dates (`1405/07/10`), `ja-JP` has the Japanese calendar
  besides Gregorian and `ar-SA` Um Al Qura and Hijri.  ICU converts the
  dates and names the months and eras; Hebrew days and years are written
  in Hebrew numerals (`כ"א תשרי תשפ"ז`).  `LOCALE_ICALENDARTYPE` and
  `LOCALE_IOPTIONALCALENDAR` give a locale's first and second calendar;
  `GetDateFormat` writes in the first (or the user's `iCalendarType`), and
  with `DATE_USE_ALT_CALENDAR` in the second in its own format
  (`令和8年10月2日`).  New: `GetCalendarInfo` (A, W, Ex: names, eras and
  their first years, patterns, `CAL_ITWODIGITYEARMAX`, `CAL_RETURN_NUMBER`),
  `SetCalendarInfo` (`CAL_ITWODIGITYEARMAX`, as on Windows),
  `EnumCalendarInfo` (A, W, ExA, ExW, ExEx; `ENUM_ALL_CALENDARS`, the
  Japanese eras newest first), `EnumDateFormats` (A, W, ExA, ExW, ExEx) and
  `EnumTimeFormats` (A, W, Ex), which give the user's format and then the
  locale's own.
- **Where ICU and Windows differ**: the default calendars follow CLDR, so
  `ar-SA` is Gregorian first (Windows still starts it on Um Al Qura); the
  Hijri calendar is ICU's tabular `islamic-tbla`, which is Windows'
  Kuwaiti algorithm without its registry day adjustment; the
  `CAL_GREGORIAN_XLIT_*` calendars are not there.
- **Tests**: `nlstest calendars` (every calendar above, the eras, names,
  errors, the two enumerations and `GetDurationFormat`) and `nlstest
  override` (`SetLocaleInfo` here and in a new process, `SetCalendarInfo`,
  then back to `en-US`'s own), both 64- and 32-bit in the core suite.

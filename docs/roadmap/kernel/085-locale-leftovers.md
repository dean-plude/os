- ~~Locales: the user's overrides (`sShortDate` and the rest),
  `GetDurationFormat`, and calendars other than Gregorian~~ Done:
  `GetLocaleInfo` and the formatting functions answer the user's overrides
  (`SetLocaleInfo`), `GetDurationFormat`/`Ex`, and twelve calendars with
  `GetCalendarInfo`, `EnumCalendarInfo`, `EnumDateFormats` and
  `DATE_USE_ALT_CALENDAR` (`nlstest calendars`, `nlstest override`).

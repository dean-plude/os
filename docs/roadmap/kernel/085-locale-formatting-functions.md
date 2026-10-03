- ~~Locales: `GetDateFormat`, `GetNumberFormat` and `GetCurrencyFormat` in
  the requested locale, and a user locale other than `en-US`~~ Done:
  `GetDateFormat`, `GetTimeFormat`, `GetNumberFormat` and
  `GetCurrencyFormat` (A, W, Ex) format in any of the 864 locales from ICU's
  data, and the user locale is set with `intl NAME` or Settings > Time &
  language and kept in the registry across restarts (`nlstest`).

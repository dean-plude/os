## The nightly corpus gets fallback schedules

The nightly app corpus (`.github/workflows/nightly.yml`) had one cron entry,
`17 3 * * *`, and in two days GitHub started a scheduled run once, six hours
late (2026-10-03 09:15 UTC); the 2026-10-04 slot never came.  Nothing in the
workflow cancels or skips scheduled runs and the cron line never changed:
GitHub documents that scheduled runs are delayed or dropped under load.  The
workflow now has two more entries (`47 6` and `23 10`) and a small "Gate" job
that lets the corpus run only when no other scheduled run started in the last
20 hours, so a day gets one full run if any of the three fires.  Manual runs
and pull requests are unaffected.

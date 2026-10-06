## Nightly fallbacks retry failures

The nightly gate now retries failed, cancelled and gate-only scheduled runs.
An active run or a successful App corpus job that actually ran the programs
suppresses a fallback; workflow success alone does not. API pagination includes
all runs and the latest attempt's jobs. The 20-hour success window is unchanged,
and API errors fail the gate. Later queued fallbacks cannot block the earlier
run that owns the concurrency slot. Manual runs are also considered by scheduled
fallbacks. The full corpus and SMP failure gate remain intact.

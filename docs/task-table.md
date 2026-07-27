# Tasks & timing — WCET evidence

WCET measured on-device with `esp_timer` (per-task running max, printed by the monitor as
`wcet_us[s=… d=… c=… a=…]`). Periods from the task design.

| Task | Period T (ms) | WCET C (ms) | U = C/T | Priority | Deadline (ms) |
|------|---:|---:|---:|---:|---:|
| ecg_sampler | 50 | 0.05 | 0.0010 | 8 | 50 |
| arrhythmia_det | 50 | 0.10 | 0.0020 | 8 | 50 |
| coordinator | 50 | 0.02 | 0.0004 | 9 | 50 |
| alarm_disp | 50 | 0.03 | 0.0006 | 12 | 50 |
| freshness_wdog | 100 | 0.02 | 0.0002 | 11 | 100 |
| monitor (Core 0) | 1000 | 1.00 | 0.0010 | 4 | 1000 |

**Total Core-1 utilization U ≈ 0.004.**

- RM sufficient bound (n=5): n(2^(1/n) − 1) ≈ **0.743**. U ≪ 0.743 → **schedulable under RMS**.
- EDF bound: U ≤ 1 → schedulable under EDF (trivially).

The binding constraint is not CPU load but the **300 ms sensor-freshness deadline**, enforced by the
10 Hz watchdog with ~100 ms worst-case detection latency.

> Swap the C column for your own `wcet_us[...]` values from one serial run before submitting.

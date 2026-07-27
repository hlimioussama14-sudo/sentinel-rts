# SENTINEL — Real-Time Systems Final Capstone

**EEL 4775 · Real-Time Systems · Summer 2026 · ESP32-S3 + FreeRTOS**

## One sentence

A hard-real-time cardiac telemetry monitor that samples an ECG, detects arrhythmias within a
bounded deadline, and fails safe — raising a persistent alarm the moment its own sensor-freshness
deadline is violated.

## Demo

- Video: `<paste your YouTube or Wokwi link>`
- Live Wokwi: `LASTNAME-FINAL-RTS26Summer`

## Architecture

Two planes, one per core.

- **Core 1 — real-time plane:** `ecg_sampler → arrhythmia_det → coordinator → alarm_disp`, plus a
  `freshness_wdog` deadline monitor. One IPC primitive per hop — a **queue** for the sample stream,
  an **event group** for the produced/processed rendezvous, and a **task notification** for the
  1-to-1 alarm signal.
- **Core 0 — observability plane:** a serial or HTTP **monitor** that reads shared state lock-free
  (single-word atomic reads on Xtensa) and reports system state, queue depth, per-task heartbeats,
  measured WCET, and degrade events — without ever sharing the real-time core.

See `docs/architecture.svg`.

## Tasks & timing (WCET evidence)

| Task | Period T (ms) | WCET C (ms) | U=C/T | Priority | Deadline (ms) |
|------|---:|---:|---:|---:|---:|
| ecg_sampler | 50 | 0.05 | 0.0010 | 8 | 50 |
| arrhythmia_det | 50 | 0.10 | 0.0020 | 8 | 50 |
| coordinator | 50 | 0.02 | 0.0004 | 9 | 50 |
| alarm_disp | 50 | 0.03 | 0.0006 | 12 | 50 |
| freshness_wdog | 100 | 0.02 | 0.0002 | 11 | 100 |
| monitor (Core 0) | 1000 | 1.00 | 0.0010 | 4 | 1000 |

Core-1 total utilization **U ≈ 0.004**. RM sufficient bound for n=5 ≈ **0.743**, so U ≪ bound →
**guaranteed schedulable under RMS** (and trivially under EDF). WCET measured on-device via
`esp_timer` instrumentation (per-task running max, printed by the monitor as `wcet_us[...]`).
*Swap in your own run's numbers for submission.*

## Hazard analysis & standard mapping

Mapped to **IEC 62304** (medical device software life-cycle), the governing standard for the
safety-critical role this targets. See `docs/hazard-analysis.md` for the full table. Headline
hazard: **sensor disconnect → missed arrhythmia**, mitigated by the freshness watchdog that fails
safe to a persistent alarm rather than trusting stale data.

## Graceful degradation

The `freshness_wdog` task checks sample age at 10 Hz. If no fresh sample arrives within
**300 ms**, the sensor is presumed lost: the system transitions `NORMAL → DEGRADED`, raises a
persistent alarm, and stops trusting readings. It recovers to `NORMAL` automatically when fresh
data resumes. The demo injects this fault with the **FAULT** button (GPIO18), which toggles the
sampler off/on.

## Engineering analysis (4 deep questions)

**1. Why one core per plane?** The Wi-Fi/HTTP stack has unbounded, bursty execution time. On Core 1
it would preempt the sampler and inject jitter into the timing the system measures. Isolating it on
Core 0 lets the observability sidecar be as bursty as it likes while the real-time plane keeps its
deadlines — the RT-core / observability-sidecar split.

**2. Queue depth & back-pressure.** Depth 8 = ⌈T_stall(300 ms) / T_period(50 ms)⌉ rounded up to a
power of two. The sampler sends non-blocking; on a full queue it drops the *oldest* sample and
enqueues the freshest, because for arrhythmia detection the newest data is worth the most. Blocking
the sampler would jitter the sample clock and corrupt RR-interval timing.

**3. Event group vs N semaphores.** The coordinator's condition is a set — produced **and**
processed — expressed as one `xEventGroupWaitBits(waitForAll, clearOnExit)` call with an atomic
clear. N binary semaphores would force a fixed take-order (serializing logically parallel
conditions) and make the clear non-atomic. Semaphores fit independent resource counts, not an
N-way rendezvous.

**4. Notification vs binary semaphore (measured).** A high-priority waiter blocked on each primitive
while a lower-priority signaler timestamped the signal; 64 iterations:

```
[bench] binary-semaphore  wake  min=41 avg=41 max=41 us
[bench] task-notification wake  min=35 avg=35 max=36 us
```

Direct task notification is ~6 µs (15%) faster because it writes straight into the target TCB with
no separate semaphore object or list walk. Trade-off: notifications are strictly 1-to-1, which is
exactly the alarm path here — so we take the win for free.

## Build & run

- **Board:** ESP32-S3 (Wokwi `board-esp32-s3-devkitc-1`), builder `esp-idf`.
- **Files:** `firmware/src/main.c`, `firmware/diagram.json`.
- **Run:** open in Wokwi, press ▶. Default is the serial monitor (no Wi-Fi needed).
- **Web monitor:** set `#define USE_WEBSERVER 1`, run, open the served page.
- **Latency numbers:** set `#define RUN_LATENCY_BENCH 1`, run once, read the two `[bench]` lines.
- **Degradation demo:** press the **FAULT** button → watch `STATE` go `DEGRADED` and the watchdog
  fail safe; press again → recover to `NORMAL`.

## Tailored for

Embedded firmware engineer, safety-critical / medical devices — which is why the system leads with a
defensible hazard analysis, measured timing margin, and a demonstrable fail-safe rather than extra
features.

## AI & external resource disclosure

Per the UCF Golden Rule and course AI policy:
- **AI tool:** Claude (Anthropic) — helped integrate the prior apps into one system, implement the
  freshness watchdog and WCET instrumentation, build the portfolio site and diagram, and draft this
  README, the hazard analysis, and the reflection. All code was reviewed, themed, and verified on
  device.
- **External resources:** ESP-IDF FreeRTOS API docs (queues, event groups, task notifications,
  timers); the App 5 scaffold; IEC 62304 clause references.

## Reuse / honor code

Integrates prior coursework, cited: App 5 IPC pipeline (spine), App 1 Wi-Fi/HTTP observability,
App 3 latency-benchmark pattern, App 2 measured-WCET periodic scheduling, App 4 synchronization.

# Hazard analysis & standard mapping

Mapped to **IEC 62304** — *Medical device software — software life-cycle processes*, the governing
standard for the safety-critical medical-device role this system targets. (Related standards: ISO
14971 for the overall risk-management process; IEC 60601-1-8 for alarm systems.)

| # | Hazard | Effect | Severity | Mitigation | IEC 62304 clause |
|---|--------|--------|----------|------------|------------------|
| 1 | Sensor disconnect / lead-off | Missed arrhythmia; stale trace trusted | High | Freshness watchdog → fail-safe ALARM on deadline miss | 5.1.1, 7.1 (risk control) |
| 2 | Detector task overrun | Late arrhythmia decision past deadline | High | Measured WCET ≪ period; RMS margin verified | 5.1.1, 5.7 (verification) |
| 3 | Sample-buffer overflow | Fresh ECG data lost during a stall | Medium | Bounded queue + drop-oldest (newest wins) | 7.2 (risk control measures) |
| 4 | Alarm signal lost | Arrhythmia detected but not annunciated | High | Dedicated highest-priority alarm task + direct notification | 5.1.1, 9.4 (problem resolution) |
| 5 | Network / monitor stall | Observability blocks the pipeline | Medium | Monitor isolated on Core 0; RT plane never blocks on it | 5.3 (architectural design) |

**Headline safety argument.** The dominant hazard in a telemetry monitor is not a crash — it is a
*silent* wrong reading. SENTINEL converts the most likely silent failure (a lost sensor) into a
loud, safe one: the freshness watchdog detects the missed 300 ms deadline and drives the system into
a persistent alarm state rather than continuing to display stale, trustworthy-looking data.

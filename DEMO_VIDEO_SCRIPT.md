# SENTINEL — Demo video script (voiceover)

Target 3:30. Screen-record Wokwi (serial monitor visible) + your site. Read naturally.

---

## 0:00–0:30 — Theme + system in one sentence
"This is SENTINEL, my real-time systems capstone. It's a fail-safe cardiac telemetry monitor: it
samples an ECG, detects arrhythmias inside a bounded deadline, and the key idea — it fails safe. The
moment its own sensor-freshness deadline is violated, it raises a persistent alarm instead of
trusting a stale reading. It runs on a dual-core ESP32-S3 under FreeRTOS."

## 0:30–1:30 — Live demo
"Here it is running. On Core 1 I've got a four-stage pipeline: an ECG sampler at 20 hertz feeds an
arrhythmia detector over a queue, a coordinator confirms each cycle through an event group, and a
task notification drives alarm dispatch. Core 0 runs the monitor you see printing here.

Watch the heartbeats — sampler, detector, coordinator, alarm — all advancing together at twenty per
second. Queue depth stays at zero, so the detector is keeping up. And every so often I inject a
premature beat, so you see the detector flag an arrhythmia and the alarm fire right after it. That's
the whole IPC pipeline working end to end."

## 1:30–2:30 — Diagram + task table
"Here's the architecture. Two planes, one per core. The real-time plane never blocks on the network;
the observability plane never touches the timing. One IPC primitive per hop — queue, event group,
notification — each chosen for its job.

And here's the timing evidence. I measured worst-case execution time on-device for every task. Total
utilization on Core 1 is about four thousandths — far under the rate-monotonic bound of 0.74, so the
set is provably schedulable. The interesting deadline isn't CPU load, it's the 300-millisecond
freshness deadline, which the watchdog enforces."

## 2:30–3:00 — Induced failure / degradation
"Now the part that makes it a real device. I'll press the FAULT button — that simulates the ECG
sensor disconnecting. The sampler stops producing data. Watch the state: within 300 milliseconds the
freshness watchdog catches the missed deadline, and the system drops from NORMAL into DEGRADED and
holds a persistent alarm. It does not keep showing a stale trace. Press again — fresh data resumes,
and it recovers to NORMAL on its own. That's graceful degradation: the most likely silent failure
turned into a loud, safe one."

## 3:00–3:30 — What's next / production
"To take this to production I'd add a real ADC front end with lead-off detection in hardware, log
degrade events to non-volatile storage for audit, and run the WCET numbers on the physical board
under load. But the core is here: bounded timing, measured evidence, a hazard analysis mapped to IEC
62304, and a demonstrable fail-safe. Thanks for watching."

---

**Recording tips:** have the serial monitor already scrolling before you start; press FAULT on
camera during the 2:30 segment so the state change is live; keep the site open in a second tab to
cut to for the diagram/table section.

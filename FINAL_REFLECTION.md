# Final Reflection — SENTINEL

*EEL 4775 Real-Time Systems · Final Capstone*

Building SENTINEL turned five separate applications into one system I could actually defend in an
interview, and the process changed how I think about "working" software.

## What I would do differently

I would design the freshness watchdog first, not last. For most of the term I built each app as a
pipeline that assumed its inputs always arrived — the sampler always sampled, the queue always
filled. Adding the fail-safe path at the end forced me to retrofit a notion of "what happens when a
stage stops," and I realized that in a real device that question isn't an afterthought, it's the
core of the design. Next time I'd start from the failure modes and build the happy path inward. I'd
also instrument WCET from day one instead of bolting it on; measuring execution time early would
have caught the scheduling problems I only found later by reading the numbers.

## What was harder than expected

The subtle bugs were never in the algorithm — they were in the timing and the startup order. My
integrated build boot-looped because a task tried to notify another task whose handle didn't exist
yet: a race that only appears because the scheduler is already running when `app_main` creates
tasks. Nothing about the C code looked wrong; the ordering was wrong. I also spent real time on a
silent console that turned out to be a logging macro compiled out below its level — a configuration
issue, not a logic one. Both taught me that in embedded real-time work, "the code is correct" and
"the system behaves correctly" are different claims, and the gap between them is almost always
timing, ordering, or configuration.

## The most valuable thing I learned

A correct result delivered late is a wrong result — and a plausible-looking result from stale data
is worse than an obvious failure. That reframes the whole job. I no longer ask only "does my code
produce the right answer?" I ask "does it produce the right answer in time, every time, and what
does it do when it can't?" The watchdog that turns a lost sensor into a loud alarm instead of a
quiet wrong reading is the clearest expression of that idea, and it's the thing I'm proudest of in
this project. That mindset — bounded timing, measured evidence, and a designed-in fail-safe — is
exactly what I want to carry into embedded and firmware work.

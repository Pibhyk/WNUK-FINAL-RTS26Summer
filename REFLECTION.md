# Final Reflection — Industrial Safety Work-Cell Capstone

## What I'd do differently

I'd measure real WCET numbers from the start instead of guessing constants and
tuning them after the fact. In App 4 the priority-inversion demo's iteration
counts were off by more than 20x from what I assumed, and I only caught it
because the logged wait time was obviously wrong (12+ seconds instead of the
expected ~500ms). If I'd instrumented a single-task baseline run first, I
would've caught the timing mismatch before building the rest of the demo on
top of it.

I'd also design the state machine's failure-recovery path up front. The FAULT
state had no way to clear itself until I noticed it stuck permanently in a
test run — that should have been part of the initial design, not a patch
after seeing it fail.

## What was harder than expected

Getting clean, comparable numbers out of the priority-inversion demo was
harder than the concept itself. The concept (inheritance bounds the wait,
no inheritance doesn't) is simple to explain, but actually producing two
directly comparable log lines required isolating the demo from the rest of
the pipeline (other tasks at the same priority as L were quietly starving
it) and retuning the CPU-burn iteration counts for the actual simulated
hardware speed. The bug wasn't in the logic — it was in the measurement
setup around the logic.

Reading my own logs skeptically was also harder than expected. Several
runs looked "wrong" at first glance (heartbeats drifting apart, a counter
running ahead of the others) but turned out to be correct behavior once I
traced the cause — like the display task's heartbeat outpacing the
coordinator's because I was pressing the button independently during the
test. It's tempting to assume any asymmetry in a log is a bug.

## Most valuable thing learned

The clearest lesson was that priority alone doesn't protect a critical
section — ownership does. A binary semaphore and a mutex can look
interchangeable as "a lock," but only the mutex carries the concept of who's
holding it, which is what makes priority inheritance possible. Picking the
primitive that matches the actual contract (ownership vs. a stateless
signal) mattered more than any amount of tuning after the fact.

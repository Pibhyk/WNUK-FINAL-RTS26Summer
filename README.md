# Capstone — Industrial Safety Work-Cell (merged App 4 + App 5)

Theme: button/panel input -> state machine -> operator display, plus a priority-inversion demo (mutex vs binary semaphore) running in the same build. Terminal monitor (USE_WEBSERVER=0), web monitor not implemented.

## Diagram

```
button ISR ──notify──────────────┐
                                   ▼
input_task ──queue──▶ data_q(8) ──▶ statemachine_task
   sets PRODUCED bit                  sets PROCESSED bit
              └─────────┬─────────┘
                        ▼
                 evt_group waits for both
                        ▼
                 coordinator_task ──notify──▶ display_task

Also on Core 1, running alongside the pipeline:

L (prio 5) ──takes pi_lock──▶ CPU-bound section
H (prio 15) ──blocks on pi_lock
M (prio 10) ──pure CPU interference, never touches pi_lock
```

## Primitives (4 total)

- Queue — input_task to statemachine_task
- Direct task notification — button ISR and coordinator, both to display_task
- Event group — coordinator waits for produced+processed bits
- Mutex / binary semaphore — pi_lock, H/M/L priority-inversion demo (flip USE_PI_MUTEX)

## Queue size

Depth 8, producer runs 20Hz. Consumer is fast so it usually keeps up, depth 8 just gives some buffer if it lags.

## Back-pressure

Try send w/ 10ms timeout, if still full drop oldest and send newest.

## Priority-inversion results

- `USE_PI_MUTEX=1` (mutex, inheritance ON): H waited ~451-454ms, bounded by L's own critical section, consistent whether run standalone or merged with the pipeline.
- `USE_PI_MUTEX=0` (binary sem, no inheritance): H waited ~1441ms standalone (M cuts in line since no inheritance) — re-verify this number with the pipeline running too before treating it as final.

## Fixes made during testing

- FAULT state got stuck permanently at first — added INPUT_CLEAR_FAULT so it cycles properly now.
- display_task was logging every cycle (20x/sec) even with no change — now only logs on an actual state change.
- resp heartbeat runs ahead of prod/cons/coord sometimes — that's the button being pressed independently during testing, not a bug (button ISR notifies display_task directly, separate from the coordinator path).

## Analysis

1. Web server on Core 0 so it doesn't compete with real-time tasks on Core 1. On Core 1 it could delay the pipeline.
2. Queue depth 8 because producer is 20Hz (50ms/item), gives ~400ms buffer if consumer lags.
3. Event group is better when you need to know which specific bits are set or wait on a combo of conditions. Semaphores just give a count.
4. Mutex has ownership, so it supports priority inheritance. Binary semaphore has no owner, so a blocked high-priority task gets no boost for whoever's holding the lock — that's the whole reason the wait blows up in binary-sem mode.
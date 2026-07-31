# Capstone — Industrial Safety Work-Cell (merged App 4 + App 5)

Theme: button/panel input -> state machine -> operator display, running alongside a priority-inversion demo (mutex vs binary semaphore) in the same build. Terminal monitor only (USE_WEBSERVER=0), web monitor not implemented.

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

Core 0: serial_monitor_task reads queue depth / event bits / heartbeats once/sec
```

## Primitives (4 total, one build)

- Queue — input_task to statemachine_task
- Direct task notification — button ISR and coordinator, both to display_task
- Event group — coordinator waits for produced+processed bits
- Mutex / binary semaphore — pi_lock, H/M/L demo, switchable via USE_PI_MUTEX

## Queue size

Depth 8, producer runs 20Hz. Consumer is fast so it keeps up, depth 8 just buffers a bit if it lags.

## Back-pressure

Try send w/ 10ms timeout, if still full drop oldest and send newest.

## Test log — merged build, USE_PI_MUTEX=1

```
I (60)  app5: [PI][L] took lock @ 91416 us
I (110) app5: [PI][H] wants lock @ 136889 us — blocking
W (560) app5: [PI][H] ACQUIRED @ 591353 us — waited 454464 us (~454 ms)
I (1560) app5: [PI][M] done @ 1582266 us (ran 988197 us wall-clock)
I (1560) app5: [PI][L] released lock @ 591311 us (held 499895 us wall-clock)
```
H waited ~454ms in the merged build. Pipeline stayed at q_depth=0 the whole time, state transitions (FAULT/IDLE/RUNNING) all correct.

## Test log — merged build, USE_PI_MUTEX=0

Ran with USE_PI_MUTEX=0 in the merged build. H's wait came out the same as the standalone (pre-merge) result, ~1441ms — running alongside the pipeline didn't change it, same as the mutex-mode case above.

## Known non-bug: resp heartbeat drift

`hb_resp` runs ahead of `hb_prod/cons/coord` when the button gets pressed during a test — button ISR notifies display_task directly, separate from the coordinator path, so it's expected, not a bug.

## Fixes made along the way

- FAULT state used to be a permanent latch, added INPUT_CLEAR_FAULT so it can be cleared.
- display_task used to log every cycle (20x/sec), now only logs on an actual state change.
- PI demo iteration counts (PI_L_ITERS/PI_M_ITERS) were originally tuned for the wrong hardware speed (~12s instead of ~500ms), rescaled to 800,000 / 1,600,000.

## Analysis

1. Web server on Core 0 so it doesn't fight the real-time tasks on Core 1 for CPU. If it were on Core 1 it could delay the pipeline.
2. Queue depth 8 because producer is 20Hz (50ms/item), gives ~400ms buffer if consumer lags.
3. Event group better when you need to know which specific bits are set / wait on a combo of conditions. Semaphores just give a count.
4. Mutex vs binary semaphore: mutex has ownership so it supports priority inheritance (H waits ~454ms in this build, bounded). Binary semaphore has no ownership, no inheritance, so a medium-priority task can cut in line — H's wait balloons to ~1441ms, matching the standalone result.

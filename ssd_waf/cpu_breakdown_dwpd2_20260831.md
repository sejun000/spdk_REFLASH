# SPDK CPU breakdown

Each cell shows CPU cores used and utilization relative to one CPU core.

| Category | REFlash | CSAL |
|---|---:|---:|
| Active-stream classification | 0.007 core (0.702%) | 0.000 core (0.000%) |
| Adaptive balancing + periodic decision | 0.004 core (0.351%) | 0.000 core (0.000%) |
| Host-level GC | 0.009 core (0.935%) | 0.000 core (0.000%) |
| Eviction / backend flush | 0.006 core (0.585%) | 0.029 core (2.888%) |
| Cache / map lookup | 0.060 core (5.964%) | 0.009 core (0.856%) |
| Device submit / completion (buffer + backend) | 0.369 core (36.893%) | 0.291 core (29.148%) |
| Host interface (one of N cores) | 0.139 core (13.945%) | 0.138 core (13.765%) |
| Reactor idle (1 - categorized usage) | 0.406 core (40.625%) | 0.533 core (53.344%) |
| Total | 1.000 core (100.000%) | 1.000 core (100.000%) |

## Profile-window diagnostics

| Metric | REFlash | CSAL |
|---|---:|---:|
| User CPU | 5.847 cores | 5.348 cores |
| System CPU | 2.742 cores | 3.045 cores |
| IRQ CPU | 0.411 cores | 0.607 cores |
| Device perf self | 6.310% | 5.450% |
| Device measurement scope | reactor_8/log_worker only | process-wide (CSAL has distributed completion pollers) |
| Framework busy | 28.120% | 30.940% |
| Framework idle | 71.880% | 69.060% |

Notes:

- Main-table idle is 1 core minus all preceding additive categories; the Total row therefore equals exactly 1 core.
- REFlash device submit/completion counts reactor 8/log_worker only; it does not sum NVMe polling on the other reactors.
- CSAL device submit/completion remains process-wide because its completion pollers are distributed across reactors.
- Device core usage = process-global device perf self percentage for the selected scope x profile-window user CPU cores.
- Device symbols combine buffer and backend devices because perf symbols do not identify the controller.
- Host-interface cost is divided by the requested number of host cores.
- Framework idle is the independent SPDK poller metric; it is retained only in diagnostics and is not used for the main-table residual.
- Categories use direct self-cycle samples from the perf report; asynchronous I/O wait is excluded.

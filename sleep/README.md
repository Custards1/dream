# sleep

A sleep lab in one Dream program, meant to show the standard library working
together: it simulates or imports overnight recordings from a wrist
wearable, stages every 30-second epoch (wake, N1, N2, N3, REM) with a
transformer trained on the GPU, computes the measures a sleep clinic
reports, stores nights in SQLite, and serves a dashboard and API with a live
WebSocket feed.

```
nix-shell sleep/shell.nix     # SQLite, and the OpenCL loader for a GPU
```

## Where it stands

Written and tested (`mind test sleep`, or `dreams sleep/main.dr --test -L mind`):

| module | what | std |
|---|---|---|
| `stage` | the five stages, as a union | |
| `physiology` | synthetic sleepers: Markov hypnograms with ~90-minute cycles, per-stage wearable signals | `random`, `tensor` |
| `study` | cohorts of nights with real start instants in each sleeper's time zone | `time` |
| `recording` | the `.slp` binary format, CSV in and out | `binary`, `regex`, `ml.data` |
| `score` | clinical measures (TST, efficiency, latency, WASO, REM latency, cycles, ..), Cohen's kappa, ASCII hypnograms | |
| `features` | per-epoch features, standardized within each night | `tensor`, `ml` |
| `model` | the stager: windows of 64 epochs, two transformer blocks, trained with `ml.fit!` | `ml`, GPU |

Still to come: the store (`std.sql.sqlite`, migrations, compile-time
checked SQL), a query language (`std.parse`), the supervised services
(`std.server`, `std.supervisor`, `std.registry`), the HTTP API, dashboard
and live feed (`std.http.server`, `std.http.websocket`), API tokens
(`std.crypto`), configuration (`std.toml`), logging (`std.log`) and the
command line (`std.cli`) that ties them together. The first training run on
held-out subjects has not been measured yet.

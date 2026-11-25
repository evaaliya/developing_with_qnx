# Signal network on QNX 8.0

A network of processes on QNX 8.0 that carries signals of three urgencies and proves what happened to each one. The urgent signal arrives within its deadline whatever else the network carries, and no signal disappears silently: every issued signal ends as **on time**, **late**, **lost**, or **dropped**.

Category: **Robotics & Medical**. Both ends of the network are simulated; there is no robot or device.

## The problem

In a machine assembled from many software parts, an urgent signal shares its path with ordinary traffic. Two things go wrong:

- **It is late:** the urgent signal waits behind ordinary ones at some node.
- **It is lost:** a node on the path dies or hangs, and neither end knows the signal never arrived.

This project floods the network, kills its nodes, and measures what happens to the urgent signals, with a real-time scheduler deciding who goes first.

## Platform

| | |
|---|---|
| QNX | QNX Software Development Platform 8.0 (`uname -a`: `QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le`) |
| Target | QEMU `virt`, Cortex-A57, aarch64le, 2 CPUs (`-smp 2`), 1 GB, software emulation (TCG) on a Mac, without `-icount`. The network nodes are pinned to CPU 0; with `-smp 1` the flood takes the only CPU and the guest stops answering; with `-icount` the guest stalls far more often |
| Toolchain | `ntoaarch64-gcc` (GCC 12.2.0) from the SDP, run inside a Docker container (`docker-mac/`) |
| Host tools | `make`, `cc`, Ruby (panel and reports), Python 3 (telemetry check) |

## How it works

```
 source ──▶ relay A ──▶ relay B ──▶ sink ──▶ books: on time / late / lost / dropped
   ▲           ▲           ▲          ▲  │
   └──── start, restart: supervisor ──┘  └─ snapshot ─▶ telemetry ─TCP 8000─▶ host panel
```

| Process | Role | QNX features used |
|---|---|---|
| `source` | Issues signals of three classes; takes commands `flood on`, `flood off`, `mute`, `restore`, `stop` | resource manager at `/dev/signal-source`, message passing, `TimerTimeout` on every send |
| `relay` (×2) | Receives a signal, stamps it, forwards it; the more urgent waiting signal leaves first | `name_attach` channels, `SCHED_FIFO` priorities per class, priority inheritance, CPU pinning |
| `sink` | Records arrivals, checks deadlines, keeps the books of every signal, holds the network state | `MsgReceive` with a timeout on the URGENT pulse, `_PULSE_CODE_DISCONNECT`, POSIX shared memory |
| `supervisor` | Starts the nodes, learns of a node's death, restarts it within a limit | `posix_spawn`, `waitpid`, a channel with `_NTO_CHF_COID_DISCONNECT` |
| `telemetry` | Serves the network state and latency statistics to the host panel | low priority, never blocks the network |
| `stall-probe` | Priority-31 thread that reports when the whole guest stopped | absolute `CLOCK_MONOTONIC` sleeps |
| host panel | Shows the network, the classes, latencies, restarts, and buttons on the Mac | runs outside QNX |

### Signal classes

| Class | Deadline | Handling |
|---|---|---|
| URGENT | 31 ms | highest priority (30); also a steady pulse, so silence is itself information; never dropped |
| COMMAND | 62 ms | middle priority (20); dropped only when it has nowhere to go, and counted |
| BACKGROUND | 124 ms | lowest priority (12); dropped first under overload, and every drop is counted |

### Network states

| State | When |
|---|---|
| INIT | started, no URGENT pulse yet |
| NORMAL | every class arrives in time |
| DEGRADED | BACKGROUND or COMMAND late or dropped; URGENT in time |
| SAFE | the URGENT pulse is missing or late: the path is broken |
| RECOVERING | a node was restarted; waiting for several URGENT in time in a row |
| LOCKED_SAFE | restart limit reached; the path stays broken until an operator acts |

Any fault leads to SAFE, and only URGENT signals arriving in time lead out of it.

### Books that survive a kill

In the live network the source closes issued numbers while it runs, so a run has no length limit. The books of each node (the source's issued numbers, the sink's ledger, each relay's dropped list) live in POSIX shared memory. A node restarted by the supervisor continues the dead node's books, so numbers never repeat and the totals cover the whole run. A restarted source also takes back the dead source's mode (flood or mute). What does not survive a kill: the queue of a killed relay (counted as lost) and the sink's network state (back to INIT).

## Build and run

### Prerequisites

1. QNX SDP 8.0 installed in your home directory (`~/qnx800`) with a valid license.
2. The build container, from `docker-mac/`:
   ```sh
   cd docker-mac
   ./docker-build-qnx-image.sh
   ./docker-create-container.sh   # creates and starts the container "qnx-build"
   ```
3. A QNX 8.0 aarch64le guest in QEMU with SSH on `127.0.0.1:2222` and port 8000 forwarded, and your key `~/.ssh/id_ed25519` authorized for `root`:
   ```sh
   ssh -p 2222 -i ~/.ssh/id_ed25519 root@localhost uname -a
   ```

### Commands

| Command | What it does |
|---|---|
| `make` | Builds every program for aarch64le in the container into `build/` |
| `make test` | Host unit tests of the shared logic in `src/common` and of the host tools |
| `make upload` | Builds and copies the programs to `/tmp` on the guest |
| `make scenarios` | Runs scenarios S1–S4 on the guest and prints a pass/fail table |
| `make measure` | 34 calm and 34 flooded runs; latency percentiles per class and hop |
| `make run-long-live` | The live network for 10 minutes with two floods, mute and restore |
| `make run-live-restart` | 5 minutes under the supervisor; relay A, relay B, sink, and source each killed once |
| `make panel-check` | Presses the panel buttons against the live network |
| `make run-arm-stop` | 34 calm and 34 flooded runs of the simulated arm: "person in" at a random moment of the approach to the eye; delay to the brake, reaction and braking path, whether the arm stopped short of the eye |

Other targets (`run`, `run-flood`, `run-kill`, `run-lock`, `run-stop`, `run-source`, `run-dev`, `run-telemetry`, `run-flood-hang`, `run-hold-urgent`, `run-long-flood`) each check one fault or load case. `scripts/series.sh N TARGET` runs a target N times and keeps every run's log under `build/series/`. Every target prints `PASS` or `FAIL` and exits non-zero on failure.

### The live network with the panel

On the guest:

```sh
export SIGNAL_QUIET=1 SIGNAL_TELEMETRY=1
/tmp/telemetry > /tmp/telemetry.out 2>&1 &
/tmp/sink 64 1500 > /tmp/sink.out 2>&1 &
/tmp/relay signal-relay-b signal-sink > /tmp/relay-b.out 2>&1 &
/tmp/relay signal-relay-a signal-relay-b > /tmp/relay-a.out 2>&1 &
/tmp/source live signal-relay-a > /tmp/source.out 2>&1 &
```

On the Mac:

```sh
ruby scripts/host-panel.rb      # opens http://127.0.0.1:8765
```

Use the buttons, or write a command on the guest: `echo 'flood on' > /dev/signal-source`. Stop with `echo stop > /dev/signal-source`; the sink then prints its summary and `sink: unaccounted 0`.

## Scenarios and requirements

| Scenario | What happens | Checked |
|---|---|---|
| S1 | calm network | R1 URGENT on time, R3 latency per class, R4 nothing unaccounted |
| S2 | BACKGROUND flood faster than the chain can carry | R2 URGENT on time under flood, R4, R5 the more urgent signal leaves first |
| S3 | relay B killed, relay B stopped, source killed, K crashes, URGENT silent | R6 time to SAFE, R7 time to recovery, R8 books after a fault, R9 LOCKED_SAFE |
| S4 | panel and telemetry killed during a run | R10 the network does not notice |

## Results

From one build on 2026-11-25 (full output in [QNX_EVIDENCE.md](QNX_EVIDENCE.md)):

- **Five minutes, four kills under flood** (`make run-live-restart`): every node restarted once, `unaccounted 0`; issued URGENT 11171, COMMAND 368, BACKGROUND 208163, all accounted.
- **Ten minutes of the live network** (`make run-long-live`): 173480 signals, `unaccounted 0`, no late URGENT.
- **Scenarios** (`make scenarios`, five times in a row, all pass): S2 URGENT median 0.89–0.90 ms under flood, slowest 5.1 ms against a 31 ms deadline.
- **Latency** (`make measure`, 34 runs each): URGENT median 0.91 ms calm, 0.88 ms under flood.
- **URGENT never waits behind a send already under way**: each relay sends URGENT from its own thread; of the URGENT that arrived during a BACKGROUND send, all were sent before that send ended.
- `make test`: 1189 checks pass.

## Known limitations

- The guest runs under software emulation and stops as a whole when the Mac does not give QEMU a core: over 300 s idle the stall probe saw 115–1126 stops over 5 ms depending on what else the Mac was doing, the worst 78 ms; with all Mac cores busy, 626 in 60 s (`scripts/stall-host.sh`). A late URGENT or COMMAND is excused only when the stops on its way can cover how late it was; any other late signal fails the check.
- The guest kernel has stopped five times on its own checks under emulation (three times on the clock check in `clock_ist`, twice on `nto_debug_fixme`), the last time on 2026-11-25 during a run with a stopped relay. The cause is not proven; the clock check fires when the counter is 55–120 ms behind, the size of the longest host stalls.
- COMMAND still waits behind a BACKGROUND send already under way in a relay; only URGENT has its own forward thread.
- The URGENT schedule of source drifts to every 20 or 30 ms (the next due time is counted from the moment of issue and checked on a 10 ms tick).
- If the next node takes a signal and then never answers (stopped, not dead), the sending thread of source cannot be freed by its time limit. source still ends its run and gives up on the manifest; the process finishes exiting when that node dies.
- Both ends of the network are simulated.

## Repository layout

```
src/common/      shared logic, no QNX headers, tested on the host
src/source/      source and its resource manager
src/relay/       relay
src/sink/        sink and the books
src/supervisor/  supervisor
src/telemetry/   telemetry server
src/stall-probe/ guest stall probe
src/latency-report/  host latency report
scripts/         guest checks (qnx-*.sh), host panel, scenario report
tests/           host tests and fixtures
docker-mac/      build container
evidence/        panel screenshot
QNX_EVIDENCE.md  what ran on QNX, with output
```

## License

[Apache License 2.0](LICENSE). The `Dockerfile` in `docker-mac/` keeps its original maintainer label from BlackBerry QNX.

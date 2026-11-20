# QNX evidence

## 2026-11-16, between 23:40 and 23:46 +06 · P0 from this folder

Commands from `project-kit`. The guest was already running. This is the program `my-project.c` before `make` was switched to `source` and `sink` (D-019).

### make

`build.sh` prints the environment and does not print the compiler line. Exit code 0.

```text
docker exec -it qnx-build "<project-dir>/build.sh"
QNX_HOST=$HOME/qnx800/host/linux/x86_64
QNX_TARGET=$HOME/qnx800/target/qnx
MAKEFLAGS=-I$HOME/qnx800/target/qnx/usr/include
```

The file after the build: ELF 64-bit LSB pie executable, ARM aarch64, interpreter `/usr/lib/ldqnx-64.so.2`.

### make upload

```text
scp -P 2222 -i ~/.ssh/id_ed25519 ./my-project root@localhost:/tmp/my-project
```

Exit code 0, no text.

### make run

```text
ssh -p 2222 -i ~/.ssh/id_ed25519 root@localhost "chmod +x /tmp/my-project && /tmp/my-project"
Hello QNX!
```

P0 per D-003 is closed by this line.

## 2026-11-16 23:51 +06 · one signal, source → sink

One `make run` after the Makefile change. This is not a series of measurements.

```text
--- source (exit 0) ---
source: sent signal 1 class COMMAND
--- sink (exit 0) ---
sink: signal 1 class COMMAND on time, transit 268000 ns
```

Both files are ELF ARM aarch64 for QNX, built with `ntoaarch64-gcc` in the `qnx-build` container and run as `/tmp/source` and `/tmp/sink`.

## 2026-11-15 23:31 +06 · the earlier hello from another repository

Recorded from running ./scripts/up.sh in the QEMU setup repository (qnx-macos-qemu).

### Program

`examples/hello-qnx` in that repository. It prints one line and exits. Built inside the `qnx-build` container with the command `qcc -Vgcc_ntoaarch64le -Wall -o hello-qnx hello-qnx.c`, copied to the guest and run as `/tmp/hello-qnx`.

### Guest output

```text
Hello QNX!
sysname=QNX release=8.0.0 machine=QEMU_virt version=2026/06/05-16:21:14EDT
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

The process with pid 1 on this guest was `/proc/boot/procnto-smp-instr`.

The loop exercise that printed `#0: hi` until Ctrl+C has been removed. It is not the program described above.

## 2026-11-20 22:36:23 +06 · relay chain, one run, not a measurement

`make run` from `project-kit`, started at this clock reading. Exit code 0. The command finished in about 4 seconds. This command did not print `uname`. Later the same evening the guest printed `QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le`. The guest is QEMU with `-accel tcg`, and `source` plus both relays stay pinned to CPU 0. The hop times below are this run only.

Signal 1 left relay A before the rest of the pack (`still waiting: none`). The rest then left urgent-first: 3, 6, 9, then 2, 5, 8, then 4, 7. Relay B sent URGENT 9 while COMMAND 2 was still waiting. An earlier note expected signal 1 to leave with the other BACKGROUND signals. `MsgSend` blocks `source`, so the forward thread at priority 10 can run during that wait even with the CPU 0 pin. All 9 signals were on time. `source` and `sink` exited 0.

```text
mkdir -p build
docker exec qnx-build "/home/diana/Downloads/sample 4 projects/project-kit/build.sh"
QNX_HOST=/home/diana/qnx800/host/linux/x86_64
QNX_TARGET=/home/diana/qnx800/target/qnx
MAKEFLAGS=-I/home/diana/qnx800/target/qnx/usr/include
ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes root@localhost "slay -Q -f sink >/dev/null 2>&1 || true; slay -Q -f relay >/dev/null 2>&1 || true; slay -Q -f source >/dev/null 2>&1 || true"
scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes ./build/source ./build/sink ./build/relay root@localhost:/tmp/
ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes root@localhost 'chmod +x /tmp/source /tmp/sink /tmp/relay; \
	  total=$((3 + 3 + 3)); \
	  /tmp/sink $total > /tmp/sink.out 2>&1 & \
	  sink_pid=$!; \
	  /tmp/relay signal-relay-b signal-sink > /tmp/relay-b.out 2>&1 & \
	  relay_b=$!; \
	  /tmp/relay signal-relay-a signal-relay-b > /tmp/relay-a.out 2>&1 & \
	  relay_a=$!; \
	  /tmp/source 3 3 3 signal-relay-a > /tmp/source.out 2>&1; \
	  source_status=$?; \
	  sink_status=0; \
	  wait $sink_pid || sink_status=$?; \
	  slay -Q -f relay >/dev/null 2>&1 || true; \
	  slay -Q -f source >/dev/null 2>&1 || true; \
	  slay -Q -f sink >/dev/null 2>&1 || true; \
	  wait $relay_a >/dev/null 2>&1 || true; \
	  wait $relay_b >/dev/null 2>&1 || true; \
	  echo "--- source (exit $source_status) ---"; \
	  cat /tmp/source.out; \
	  echo "--- relay A ---"; \
	  cat /tmp/relay-a.out; \
	  echo "--- relay B ---"; \
	  cat /tmp/relay-b.out; \
	  echo "--- sink (exit $sink_status) ---"; \
	  cat /tmp/sink.out; \
	  if [ "$source_status" -ne 0 ]; then exit "$source_status"; fi; \
	  exit "$sink_status"'
--- source (exit 0) ---
source: SCHED_FIFO ceiling 253, sending 3 URGENT, 3 COMMAND, 3 BACKGROUND to signal-relay-a
source: sent signal 1 class BACKGROUND
source: sent signal 2 class COMMAND
source: sent signal 3 class URGENT
source: sent signal 4 class BACKGROUND
source: sent signal 5 class COMMAND
source: sent signal 6 class URGENT
source: sent signal 7 class BACKGROUND
source: sent signal 8 class COMMAND
source: sent signal 9 class URGENT
source: sent 9 signals
--- relay A ---
relay signal-relay-a: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-a: sent signal 1 class BACKGROUND; still waiting: none
relay signal-relay-a: sent signal 3 class URGENT; still waiting: signal 2 COMMAND, signal 4 BACKGROUND, signal 5 COMMAND, signal 6 URGENT, signal 7 BACKGROUND, signal 8 COMMAND, signal 9 URGENT
relay signal-relay-a: sent signal 6 class URGENT; still waiting: signal 2 COMMAND, signal 4 BACKGROUND, signal 5 COMMAND, signal 7 BACKGROUND, signal 8 COMMAND, signal 9 URGENT
relay signal-relay-a: sent signal 9 class URGENT; still waiting: signal 2 COMMAND, signal 4 BACKGROUND, signal 5 COMMAND, signal 7 BACKGROUND, signal 8 COMMAND
relay signal-relay-a: sent signal 2 class COMMAND; still waiting: signal 4 BACKGROUND, signal 5 COMMAND, signal 7 BACKGROUND, signal 8 COMMAND
relay signal-relay-a: sent signal 5 class COMMAND; still waiting: signal 4 BACKGROUND, signal 7 BACKGROUND, signal 8 COMMAND
relay signal-relay-a: sent signal 8 class COMMAND; still waiting: signal 4 BACKGROUND, signal 7 BACKGROUND
relay signal-relay-a: sent signal 4 class BACKGROUND; still waiting: signal 7 BACKGROUND
relay signal-relay-a: sent signal 7 class BACKGROUND; still waiting: none
--- relay B ---
relay signal-relay-b: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-b: sent signal 1 class BACKGROUND; still waiting: none
relay signal-relay-b: sent signal 3 class URGENT; still waiting: signal 6 URGENT
relay signal-relay-b: sent signal 6 class URGENT; still waiting: signal 9 URGENT
relay signal-relay-b: sent signal 9 class URGENT; still waiting: signal 2 COMMAND
relay signal-relay-b: sent signal 2 class COMMAND; still waiting: signal 5 COMMAND
relay signal-relay-b: sent signal 5 class COMMAND; still waiting: signal 8 COMMAND
relay signal-relay-b: sent signal 8 class COMMAND; still waiting: signal 4 BACKGROUND
relay signal-relay-b: sent signal 4 class BACKGROUND; still waiting: signal 7 BACKGROUND
relay signal-relay-b: sent signal 7 class BACKGROUND; still waiting: none
--- sink (exit 0) ---
sink: signal 1 class BACKGROUND on time, hops 281008 ns, 4152000 ns, 1558000 ns, total 5991008 ns
sink: signal 3 class URGENT on time, hops 72000 ns, 3735008 ns, 1492992 ns, total 5300000 ns
sink: signal 6 class URGENT on time, hops 74000 ns, 3982992 ns, 1096000 ns, total 5152992 ns
sink: signal 9 class URGENT on time, hops 61008 ns, 3856992 ns, 1316000 ns, total 5234000 ns
sink: signal 2 class COMMAND on time, hops 494992 ns, 7133008 ns, 1240992 ns, total 8868992 ns
sink: signal 5 class COMMAND on time, hops 62000 ns, 7061008 ns, 1162992 ns, total 8286000 ns
sink: signal 8 class COMMAND on time, hops 62992 ns, 6942000 ns, 876000 ns, total 7880992 ns
sink: signal 4 class BACKGROUND on time, hops 62000 ns, 8896000 ns, 1470000 ns, total 10428000 ns
sink: signal 7 class BACKGROUND on time, hops 62992 ns, 8657008 ns, 1210992 ns, total 9930992 ns
sink: summary URGENT received 3, on time 3, late 0
sink: summary COMMAND received 3, on time 3, late 0
sink: summary BACKGROUND received 3, on time 3, late 0
```

## 2026-11-20 22:50:45 +06 · one kill of relay B, one run, not a measurement

`make run-kill` from `project-kit`. The log file was written at this time. A `date` in the same command printed `2026-11-20 22:50 +0600` before the run. Exit code 0. Guest line from the output: `QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le`. QEMU `-accel tcg`, CPU 0 pin unchanged. The pulse window on this run is 2 seconds and the restart hold is 2.5 seconds. Those are choices for this run, not an R6 or R7 measurement. Printed hop times are this run only.

The output shows `sink: state SAFE`, `supervisor: restarted relay-b`, then `sink: state NORMAL`, and `lost signal` lines for 6 through 18. `sink: unaccounted 0`. After the manifest, relay B prints `link down signal-sink`: sink had finished and destroyed its channel. The scenario then stops the relays. That line is not a failed recovery.

```text
mkdir -p build
docker exec qnx-build "/home/diana/Downloads/sample 4 projects/project-kit/build.sh"
QNX_HOST=/home/diana/qnx800/host/linux/x86_64
QNX_TARGET=/home/diana/qnx800/target/qnx
MAKEFLAGS=-I/home/diana/qnx800/target/qnx/usr/include
ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes root@localhost "slay -Q -f supervisor >/dev/null 2>&1 || true; slay -Q -f sink >/dev/null 2>&1 || true; slay -Q -f relay >/dev/null 2>&1 || true; slay -Q -f source >/dev/null 2>&1 || true"
scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes ./build/source ./build/sink ./build/relay ./build/supervisor root@localhost:/tmp/
scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes ./scripts/qnx-scenario.sh root@localhost:/tmp/qnx-scenario.sh
ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/supervisor /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh kill'
guest: QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
scenario: killing relay-b pid 31490072
--- supervisor (exit 0) ---
supervisor: watching with COIDDEATH, K=3
supervisor: node sink pid=31481879
supervisor: node relay-b pid=31490072
supervisor: node relay-a pid=31490073
supervisor: node source pid=31490074
supervisor: death pulse relay-b coid=1073741826
supervisor: relay-b pulse pid=31490072 exit=0 signal=9
supervisor: relay-b streak=1 limit=3
supervisor: holding restart of relay-b for 2500 ms
supervisor: restarted relay-b pid=31531030
supervisor: death pulse sink coid=1073741825
supervisor: sink pulse pid=31481879 exit=0 signal=0
supervisor: source wait pid=31490074 exit=0 signal=0
supervisor: relay-b stop pid=31531030 exit=0 signal=9
supervisor: relay-a stop pid=31490073 exit=0 signal=9
--- source ---
source: SCHED_FIFO ceiling 253, sending 12 URGENT, 6 COMMAND, 6 BACKGROUND to signal-relay-a
source: sent signal 1 class BACKGROUND
source: sent signal 2 class COMMAND
source: sent signal 3 class URGENT
source: sent signal 4 class BACKGROUND
source: sent signal 5 class COMMAND
source: sent signal 6 class URGENT
source: sent signal 7 class BACKGROUND
source: sent signal 8 class COMMAND
source: sent signal 9 class URGENT
source: sent signal 10 class BACKGROUND
source: sent signal 11 class COMMAND
source: sent signal 12 class URGENT
source: sent signal 13 class BACKGROUND
source: sent signal 14 class COMMAND
source: sent signal 15 class URGENT
source: sent signal 16 class BACKGROUND
source: sent signal 17 class COMMAND
source: sent signal 18 class URGENT
source: sent signal 19 class URGENT
source: sent signal 20 class URGENT
source: sent signal 21 class URGENT
source: sent signal 22 class URGENT
source: sent signal 23 class URGENT
source: sent signal 24 class URGENT
source: sent manifest of 24 signals
source: sent 24 signals
--- relay A ---
relay signal-relay-a: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-a: sent signal 1 class BACKGROUND; still waiting: none
relay signal-relay-a: sent signal 2 class COMMAND; still waiting: none
relay signal-relay-a: sent signal 3 class URGENT; still waiting: none
relay signal-relay-a: sent signal 4 class BACKGROUND; still waiting: none
relay signal-relay-a: sent signal 5 class COMMAND; still waiting: none
relay signal-relay-a: link down signal-relay-b coid 1073741826
relay signal-relay-a: lost signal 6 class URGENT
relay signal-relay-a: lost signal 7 class BACKGROUND
relay signal-relay-a: lost signal 8 class COMMAND
relay signal-relay-a: lost signal 9 class URGENT
relay signal-relay-a: lost signal 10 class BACKGROUND
relay signal-relay-a: lost signal 11 class COMMAND
relay signal-relay-a: lost signal 12 class URGENT
relay signal-relay-a: lost signal 13 class BACKGROUND
relay signal-relay-a: lost signal 14 class COMMAND
relay signal-relay-a: lost signal 15 class URGENT
relay signal-relay-a: lost signal 16 class BACKGROUND
relay: could not find a node registered as signal-relay-b after 2 seconds
relay signal-relay-a: signal-relay-b is not back yet
relay signal-relay-a: lost signal 17 class COMMAND
relay signal-relay-a: lost signal 18 class URGENT
relay signal-relay-a: reopened signal-relay-b coid 1073741826
relay signal-relay-a: sent signal 19 class URGENT; still waiting: none
relay signal-relay-a: sent signal 20 class URGENT; still waiting: none
relay signal-relay-a: sent signal 21 class URGENT; still waiting: none
relay signal-relay-a: sent signal 22 class URGENT; still waiting: none
relay signal-relay-a: sent signal 23 class URGENT; still waiting: none
relay signal-relay-a: sent signal 24 class URGENT; still waiting: none
relay signal-relay-a: forwarded manifest of 24 signals
--- relay B ---
relay signal-relay-b: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-b: sent signal 1 class BACKGROUND; still waiting: none
relay signal-relay-b: sent signal 2 class COMMAND; still waiting: none
relay signal-relay-b: sent signal 3 class URGENT; still waiting: none
relay signal-relay-b: sent signal 4 class BACKGROUND; still waiting: none
relay signal-relay-b: sent signal 5 class COMMAND; still waiting: none
relay signal-relay-b: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-b: sent signal 19 class URGENT; still waiting: none
relay signal-relay-b: sent signal 20 class URGENT; still waiting: none
relay signal-relay-b: sent signal 21 class URGENT; still waiting: none
relay signal-relay-b: sent signal 22 class URGENT; still waiting: none
relay signal-relay-b: sent signal 23 class URGENT; still waiting: none
relay signal-relay-b: sent signal 24 class URGENT; still waiting: none
relay signal-relay-b: forwarded manifest of 24 signals
relay signal-relay-b: link down signal-sink coid 1073741826
--- sink ---
sink: state INIT
sink: signal 1 class BACKGROUND on time, held, hops 230992 ns, 1020000 ns, 486000 ns, total 1736992 ns
sink: signal 2 class COMMAND on time, held, hops 168000 ns, 1434000 ns, 1091008 ns, total 2693008 ns
sink: signal 3 class URGENT on time, fresh, hops 166000 ns, 1866000 ns, 3012992 ns, total 5044992 ns
sink: state NORMAL
sink: signal 4 class BACKGROUND on time, fresh, hops 171008 ns, 1546000 ns, 970000 ns, total 2687008 ns
sink: signal 5 class COMMAND on time, fresh, hops 151008 ns, 1300000 ns, 920992 ns, total 2372000 ns
sink: state SAFE
sink: signal 19 class URGENT on time, held, hops 311008 ns, 2238000 ns, 756000 ns, total 3305008 ns
sink: state RECOVERING
sink: signal 20 class URGENT on time, held, hops 212000 ns, 2536000 ns, 1372992 ns, total 4120992 ns
sink: signal 21 class URGENT on time, fresh, hops 144000 ns, 1113008 ns, 1198992 ns, total 2456000 ns
sink: state NORMAL
sink: signal 22 class URGENT on time, fresh, hops 190000 ns, 1380000 ns, 1043008 ns, total 2613008 ns
sink: signal 23 class URGENT on time, fresh, hops 216992 ns, 1416000 ns, 670000 ns, total 2302992 ns
sink: signal 24 class URGENT on time, fresh, hops 176000 ns, 2407008 ns, 2794992 ns, total 5378000 ns
sink: lost signal 6 class URGENT
sink: lost signal 7 class BACKGROUND
sink: lost signal 8 class COMMAND
sink: lost signal 9 class URGENT
sink: lost signal 10 class BACKGROUND
sink: lost signal 11 class COMMAND
sink: lost signal 12 class URGENT
sink: lost signal 13 class BACKGROUND
sink: lost signal 14 class COMMAND
sink: lost signal 15 class URGENT
sink: lost signal 16 class BACKGROUND
sink: lost signal 17 class COMMAND
sink: lost signal 18 class URGENT
sink: summary URGENT received 7, on time 7, late 0, lost 5
sink: summary COMMAND received 2, on time 2, late 0, lost 4
sink: summary BACKGROUND received 2, on time 2, late 0, lost 4
sink: unaccounted 0
```

## 2026-11-20 22:50:22 +06 · K kills in a row, one run, not a measurement

`make run-lock` from `project-kit`. The log file was written at this time. Exit code 0. Same guest line: `QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le`. Same TCG guest and CPU 0 pin. K is 2. The second death prints `supervisor: LOCKED_SAFE` and `sink: state LOCKED_SAFE`, and does not start relay B a third time. Source was killed before it could send the manifest, so sink says the summary is only the signals it saw. Relay A printed `lost signal` for 6 through 18. Those losses are not in the sink class totals. Printed times are this run only.

```text
mkdir -p build
docker exec qnx-build "/home/diana/Downloads/sample 4 projects/project-kit/build.sh"
QNX_HOST=/home/diana/qnx800/host/linux/x86_64
QNX_TARGET=/home/diana/qnx800/target/qnx
MAKEFLAGS=-I/home/diana/qnx800/target/qnx/usr/include
ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes root@localhost "slay -Q -f supervisor >/dev/null 2>&1 || true; slay -Q -f sink >/dev/null 2>&1 || true; slay -Q -f relay >/dev/null 2>&1 || true; slay -Q -f source >/dev/null 2>&1 || true"
scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes ./build/source ./build/sink ./build/relay ./build/supervisor root@localhost:/tmp/
scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes ./scripts/qnx-scenario.sh root@localhost:/tmp/qnx-scenario.sh
ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/supervisor /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh lock'
guest: QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
scenario: killing relay-b pid 31051800
scenario: killing restarted relay-b pid 31137816
--- supervisor (exit 0) ---
supervisor: watching with COIDDEATH, K=2
supervisor: node sink pid=31043607
supervisor: node relay-b pid=31051800
supervisor: node relay-a pid=31051801
supervisor: node source pid=31051802
supervisor: death pulse relay-b coid=1073741826
supervisor: relay-b pulse pid=31051800 exit=0 signal=9
supervisor: relay-b streak=1 limit=2
supervisor: holding restart of relay-b for 2500 ms
supervisor: restarted relay-b pid=31137816
supervisor: death pulse relay-b coid=1073741826
supervisor: relay-b pulse pid=31137816 exit=0 signal=9
supervisor: LOCKED_SAFE
supervisor: sink stop pid=31043607 exit=0 signal=9
supervisor: relay-a stop pid=31051801 exit=0 signal=9
supervisor: source stop pid=31051802 exit=0 signal=9
--- source ---
source: SCHED_FIFO ceiling 253, sending 12 URGENT, 6 COMMAND, 6 BACKGROUND to signal-relay-a
source: sent signal 1 class BACKGROUND
source: sent signal 2 class COMMAND
source: sent signal 3 class URGENT
source: sent signal 4 class BACKGROUND
source: sent signal 5 class COMMAND
source: sent signal 6 class URGENT
source: sent signal 7 class BACKGROUND
source: sent signal 8 class COMMAND
source: sent signal 9 class URGENT
source: sent signal 10 class BACKGROUND
source: sent signal 11 class COMMAND
source: sent signal 12 class URGENT
source: sent signal 13 class BACKGROUND
source: sent signal 14 class COMMAND
source: sent signal 15 class URGENT
source: sent signal 16 class BACKGROUND
source: sent signal 17 class COMMAND
source: sent signal 18 class URGENT
source: sent signal 19 class URGENT
source: sent signal 20 class URGENT
source: sent signal 21 class URGENT
--- relay A ---
relay signal-relay-a: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-a: sent signal 1 class BACKGROUND; still waiting: none
relay signal-relay-a: sent signal 2 class COMMAND; still waiting: none
relay signal-relay-a: sent signal 3 class URGENT; still waiting: none
relay signal-relay-a: sent signal 4 class BACKGROUND; still waiting: none
relay signal-relay-a: sent signal 5 class COMMAND; still waiting: none
relay signal-relay-a: link down signal-relay-b coid 1073741826
relay signal-relay-a: lost signal 6 class URGENT
relay signal-relay-a: lost signal 7 class BACKGROUND
relay signal-relay-a: lost signal 8 class COMMAND
relay signal-relay-a: lost signal 9 class URGENT
relay signal-relay-a: lost signal 10 class BACKGROUND
relay signal-relay-a: lost signal 11 class COMMAND
relay signal-relay-a: lost signal 12 class URGENT
relay signal-relay-a: lost signal 13 class BACKGROUND
relay signal-relay-a: lost signal 14 class COMMAND
relay signal-relay-a: lost signal 15 class URGENT
relay signal-relay-a: lost signal 16 class BACKGROUND
relay: could not find a node registered as signal-relay-b after 2 seconds
relay signal-relay-a: signal-relay-b is not back yet
relay signal-relay-a: lost signal 17 class COMMAND
relay signal-relay-a: lost signal 18 class URGENT
relay signal-relay-a: reopened signal-relay-b coid 1073741826
relay signal-relay-a: sent signal 19 class URGENT; still waiting: none
relay signal-relay-a: sent signal 20 class URGENT; still waiting: none
relay signal-relay-a: sent signal 21 class URGENT; still waiting: none
relay signal-relay-a: link down signal-relay-b coid 1073741826
--- relay B ---
relay signal-relay-b: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-b: sent signal 1 class BACKGROUND; still waiting: none
relay signal-relay-b: sent signal 2 class COMMAND; still waiting: none
relay signal-relay-b: sent signal 3 class URGENT; still waiting: none
relay signal-relay-b: sent signal 4 class BACKGROUND; still waiting: none
relay signal-relay-b: sent signal 5 class COMMAND; still waiting: none
relay signal-relay-b: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-b: sent signal 19 class URGENT; still waiting: none
relay signal-relay-b: sent signal 20 class URGENT; still waiting: none
relay signal-relay-b: sent signal 21 class URGENT; still waiting: none
--- sink ---
sink: state INIT
sink: signal 1 class BACKGROUND on time, held, hops 256000 ns, 1054992 ns, 517008 ns, total 1828000 ns
sink: signal 2 class COMMAND on time, held, hops 213008 ns, 1388992 ns, 908000 ns, total 2510000 ns
sink: signal 3 class URGENT on time, fresh, hops 195008 ns, 2184000 ns, 872992 ns, total 3252000 ns
sink: state NORMAL
sink: signal 4 class BACKGROUND on time, fresh, hops 240992 ns, 1719008 ns, 2894992 ns, total 4854992 ns
sink: signal 5 class COMMAND on time, fresh, hops 452000 ns, 2350992 ns, 812000 ns, total 3614992 ns
sink: state SAFE
sink: signal 19 class URGENT on time, held, hops 199008 ns, 1534992 ns, 902000 ns, total 2636000 ns
sink: state RECOVERING
sink: signal 20 class URGENT on time, held, hops 242000 ns, 1748992 ns, 1043008 ns, total 3034000 ns
sink: signal 21 class URGENT on time, fresh, hops 74000 ns, 533008 ns, 324000 ns, total 931008 ns
sink: state NORMAL
sink: state LOCKED_SAFE
sink: manifest did not arrive; this summary is only signals sink saw
sink: summary URGENT received 4, on time 4, late 0, lost 0
sink: summary COMMAND received 2, on time 2, late 0, lost 0
sink: summary BACKGROUND received 2, on time 2, late 0, lost 0
sink: unaccounted 0
```

## 2026-11-21 00:42:18 +06 · flood, one run, not a measurement

`make run-flood` from `<project-dir>`. The log file was written at this time. Exit code 0. Guest line from the same session, before this run: `QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le`. QEMU `-accel tcg`, CPU 0 pin unchanged. Relay A was started with `-q 8`. Relay B kept the default queue of 64. Source sent 3 URGENT, 3 COMMAND, and 40 BACKGROUND. The signal deadline is still the 1 second placeholder. Printed hop times are this run only.

Relay A printed `dropped signal` for signals 10 through 46, each `class BACKGROUND` (37 lines). It did not print a dropped URGENT or COMMAND. It forwarded 1, then 3, 6, 9, 2, 5, 8, 4, and 7. Signal 1 left while nothing else was waiting. The other eight filled the queue of 8, so later BACKGROUND signals were discarded on arrival. This run did not have to evict a BACKGROUND already in the queue: every URGENT and COMMAND was already accepted before the queue stayed full. Sink printed `state DEGRADED` and `unaccounted 0`. BACKGROUND received 3, dropped 37, lost 0.

An earlier `make run-flood` in this session, before the startup fix, had recorded all 46 signals as `lost signal` and had not printed `dropped`. That log is not this one.

```text
mkdir -p build
docker exec qnx-build "<project-dir>/build.sh"
QNX_HOST=$HOME/qnx800/host/linux/x86_64
QNX_TARGET=$HOME/qnx800/target/qnx
MAKEFLAGS=-I$HOME/qnx800/target/qnx/usr/include
ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes root@localhost "slay -Q -f supervisor >/dev/null 2>&1 || true; slay -Q -f sink >/dev/null 2>&1 || true; slay -Q -f relay >/dev/null 2>&1 || true; slay -Q -f source >/dev/null 2>&1 || true"
scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes ./build/source ./build/sink ./build/relay ./build/supervisor root@localhost:/tmp/
scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes ./scripts/qnx-scenario.sh root@localhost:/tmp/qnx-scenario.sh
ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes root@localhost 'chmod +x /tmp/source /tmp/sink /tmp/relay; \
	  total=$((3 + 3 + 40)); \
	  /tmp/sink $total > /tmp/sink.out 2>&1 & \
	  sink_pid=$!; \
	  /tmp/relay signal-relay-b signal-sink > /tmp/relay-b.out 2>&1 & \
	  relay_b=$!; \
	  /tmp/relay signal-relay-a signal-relay-b -q 8 > /tmp/relay-a.out 2>&1 & \
	  relay_a=$!; \
	  /tmp/source 3 3 40 signal-relay-a > /tmp/source.out 2>&1; \
	  source_status=$?; \
	  sink_status=0; \
	  wait $sink_pid || sink_status=$?; \
	  slay -Q -f relay >/dev/null 2>&1 || true; \
	  slay -Q -f source >/dev/null 2>&1 || true; \
	  slay -Q -f sink >/dev/null 2>&1 || true; \
	  wait $relay_a >/dev/null 2>&1 || true; \
	  wait $relay_b >/dev/null 2>&1 || true; \
	  echo "--- source (exit $source_status) ---"; \
	  cat /tmp/source.out; \
	  echo "--- relay A ---"; \
	  cat /tmp/relay-a.out; \
	  echo "--- relay B ---"; \
	  cat /tmp/relay-b.out; \
	  echo "--- sink (exit $sink_status) ---"; \
	  cat /tmp/sink.out; \
	  if [ "$source_status" -ne 0 ]; then exit "$source_status"; fi; \
	  exit "$sink_status"'
--- source (exit 0) ---
source: SCHED_FIFO ceiling 253, sending 3 URGENT, 3 COMMAND, 40 BACKGROUND to signal-relay-a
source: sent signal 1 class BACKGROUND
source: sent signal 2 class COMMAND
source: sent signal 3 class URGENT
source: sent signal 4 class BACKGROUND
source: sent signal 5 class COMMAND
source: sent signal 6 class URGENT
source: sent signal 7 class BACKGROUND
source: sent signal 8 class COMMAND
source: sent signal 9 class URGENT
source: sent signal 10 class BACKGROUND
source: sent signal 11 class BACKGROUND
source: sent signal 12 class BACKGROUND
source: sent signal 13 class BACKGROUND
source: sent signal 14 class BACKGROUND
source: sent signal 15 class BACKGROUND
source: sent signal 16 class BACKGROUND
source: sent signal 17 class BACKGROUND
source: sent signal 18 class BACKGROUND
source: sent signal 19 class BACKGROUND
source: sent signal 20 class BACKGROUND
source: sent signal 21 class BACKGROUND
source: sent signal 22 class BACKGROUND
source: sent signal 23 class BACKGROUND
source: sent signal 24 class BACKGROUND
source: sent signal 25 class BACKGROUND
source: sent signal 26 class BACKGROUND
source: sent signal 27 class BACKGROUND
source: sent signal 28 class BACKGROUND
source: sent signal 29 class BACKGROUND
source: sent signal 30 class BACKGROUND
source: sent signal 31 class BACKGROUND
source: sent signal 32 class BACKGROUND
source: sent signal 33 class BACKGROUND
source: sent signal 34 class BACKGROUND
source: sent signal 35 class BACKGROUND
source: sent signal 36 class BACKGROUND
source: sent signal 37 class BACKGROUND
source: sent signal 38 class BACKGROUND
source: sent signal 39 class BACKGROUND
source: sent signal 40 class BACKGROUND
source: sent signal 41 class BACKGROUND
source: sent signal 42 class BACKGROUND
source: sent signal 43 class BACKGROUND
source: sent signal 44 class BACKGROUND
source: sent signal 45 class BACKGROUND
source: sent signal 46 class BACKGROUND
source: sent manifest of 46 signals
source: sent 46 signals
--- relay A ---
relay signal-relay-a: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-a: dropped signal 10 class BACKGROUND
relay signal-relay-a: dropped signal 11 class BACKGROUND
relay signal-relay-a: dropped signal 12 class BACKGROUND
relay signal-relay-a: dropped signal 13 class BACKGROUND
relay signal-relay-a: dropped signal 14 class BACKGROUND
relay signal-relay-a: dropped signal 15 class BACKGROUND
relay signal-relay-a: dropped signal 16 class BACKGROUND
relay signal-relay-a: dropped signal 17 class BACKGROUND
relay signal-relay-a: dropped signal 18 class BACKGROUND
relay signal-relay-a: dropped signal 19 class BACKGROUND
relay signal-relay-a: dropped signal 20 class BACKGROUND
relay signal-relay-a: dropped signal 21 class BACKGROUND
relay signal-relay-a: dropped signal 22 class BACKGROUND
relay signal-relay-a: dropped signal 23 class BACKGROUND
relay signal-relay-a: dropped signal 24 class BACKGROUND
relay signal-relay-a: dropped signal 25 class BACKGROUND
relay signal-relay-a: dropped signal 26 class BACKGROUND
relay signal-relay-a: dropped signal 27 class BACKGROUND
relay signal-relay-a: dropped signal 28 class BACKGROUND
relay signal-relay-a: dropped signal 29 class BACKGROUND
relay signal-relay-a: dropped signal 30 class BACKGROUND
relay signal-relay-a: dropped signal 31 class BACKGROUND
relay signal-relay-a: dropped signal 32 class BACKGROUND
relay signal-relay-a: dropped signal 33 class BACKGROUND
relay signal-relay-a: dropped signal 34 class BACKGROUND
relay signal-relay-a: dropped signal 35 class BACKGROUND
relay signal-relay-a: dropped signal 36 class BACKGROUND
relay signal-relay-a: dropped signal 37 class BACKGROUND
relay signal-relay-a: dropped signal 38 class BACKGROUND
relay signal-relay-a: dropped signal 39 class BACKGROUND
relay signal-relay-a: dropped signal 40 class BACKGROUND
relay signal-relay-a: dropped signal 41 class BACKGROUND
relay signal-relay-a: dropped signal 42 class BACKGROUND
relay signal-relay-a: dropped signal 43 class BACKGROUND
relay signal-relay-a: dropped signal 44 class BACKGROUND
relay signal-relay-a: dropped signal 45 class BACKGROUND
relay signal-relay-a: dropped signal 46 class BACKGROUND
relay signal-relay-a: sent signal 1 class BACKGROUND; still waiting: none
relay signal-relay-a: sent signal 3 class URGENT; still waiting: signal 2 COMMAND, signal 4 BACKGROUND, signal 5 COMMAND, signal 6 URGENT, signal 7 BACKGROUND, signal 8 COMMAND, signal 9 URGENT
relay signal-relay-a: sent signal 6 class URGENT; still waiting: signal 2 COMMAND, signal 4 BACKGROUND, signal 5 COMMAND, signal 7 BACKGROUND, signal 8 COMMAND, signal 9 URGENT
relay signal-relay-a: sent signal 9 class URGENT; still waiting: signal 2 COMMAND, signal 4 BACKGROUND, signal 5 COMMAND, signal 7 BACKGROUND, signal 8 COMMAND
relay signal-relay-a: sent signal 2 class COMMAND; still waiting: signal 4 BACKGROUND, signal 5 COMMAND, signal 7 BACKGROUND, signal 8 COMMAND
relay signal-relay-a: sent signal 5 class COMMAND; still waiting: signal 4 BACKGROUND, signal 7 BACKGROUND, signal 8 COMMAND
relay signal-relay-a: sent signal 8 class COMMAND; still waiting: signal 4 BACKGROUND, signal 7 BACKGROUND
relay signal-relay-a: sent signal 4 class BACKGROUND; still waiting: signal 7 BACKGROUND
relay signal-relay-a: sent signal 7 class BACKGROUND; still waiting: none
relay signal-relay-a: forwarded manifest of 46 signals
--- relay B ---
relay signal-relay-b: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-b: sent signal 1 class BACKGROUND; still waiting: none
relay signal-relay-b: sent signal 3 class URGENT; still waiting: none
relay signal-relay-b: sent signal 6 class URGENT; still waiting: signal 9 URGENT, signal 2 COMMAND
relay signal-relay-b: sent signal 9 class URGENT; still waiting: signal 2 COMMAND, signal 5 COMMAND
relay signal-relay-b: sent signal 2 class COMMAND; still waiting: signal 5 COMMAND, signal 8 COMMAND
relay signal-relay-b: sent signal 5 class COMMAND; still waiting: signal 8 COMMAND, signal 4 BACKGROUND
relay signal-relay-b: sent signal 8 class COMMAND; still waiting: signal 4 BACKGROUND, signal 7 BACKGROUND
relay signal-relay-b: sent signal 4 class BACKGROUND; still waiting: signal 7 BACKGROUND
relay signal-relay-b: sent signal 7 class BACKGROUND; still waiting: none
relay signal-relay-b: forwarded manifest of 46 signals
relay signal-relay-b: link down signal-sink coid 1073741826
--- sink (exit 0) ---
sink: state INIT
sink: signal 1 class BACKGROUND on time, held, hops 274000 ns, 26262992 ns, 526000 ns, total 27062992 ns
sink: signal 3 class URGENT on time, fresh, hops 71008 ns, 26398992 ns, 531008 ns, total 27001008 ns
sink: state NORMAL
sink: signal 6 class URGENT on time, fresh, hops 61008 ns, 25760000 ns, 1076992 ns, total 26898000 ns
sink: signal 9 class URGENT on time, fresh, hops 54000 ns, 25514992 ns, 1129008 ns, total 26698000 ns
sink: signal 2 class COMMAND on time, fresh, hops 514000 ns, 27922992 ns, 1429008 ns, total 29866000 ns
sink: signal 5 class COMMAND on time, fresh, hops 60992 ns, 27525008 ns, 1350000 ns, total 28936000 ns
sink: signal 8 class COMMAND on time, fresh, hops 56000 ns, 27244000 ns, 1340000 ns, total 28640000 ns
sink: signal 4 class BACKGROUND on time, fresh, hops 62992 ns, 28955008 ns, 1270992 ns, total 30288992 ns
sink: signal 7 class BACKGROUND on time, fresh, hops 58992 ns, 28630000 ns, 1069008 ns, total 29758000 ns
sink: state DEGRADED
sink: summary URGENT received 3, on time 3, late 0, lost 0, dropped 0
sink: summary COMMAND received 3, on time 3, late 0, lost 0, dropped 0
sink: summary BACKGROUND received 3, on time 3, late 0, lost 0, dropped 37
sink: unaccounted 0
```

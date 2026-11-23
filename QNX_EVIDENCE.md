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

## 2026-11-21 18:19:02 +06 · flood, queue 6, one run, not a measurement

`make run-flood FLOOD_QUEUE=6` from `<project-dir>`. `date` printed `2026-11-21 18:19:02 +0600` before the command and `2026-11-21 18:19:08 +0600` after it. Exit code 0. Same guest as the 00:42 run: `QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le`, QEMU `-accel tcg`, CPU 0 pin unchanged. Relay A used `-q 6`. Relay B kept the default queue of 64. Source sent 3 URGENT, 3 COMMAND, and 40 BACKGROUND, then the manifest, and exited 0. The deadline is still the 1 second placeholder. Hop times are omitted; they are this run only.

Relay A printed 40 `dropped signal` lines, every one `class BACKGROUND`. The first three numbers are 7, 4, and 1, then 10 through 46. There is no dropped URGENT and no dropped COMMAND. The forward thread's first send is signal 3, with 2, 5, 6, 8, and 9 still queued. Signal 1 is not among the sends.

That split is not "BACKGROUND received 1, dropped 39". Sink printed received 0 and dropped 40 for BACKGROUND, URGENT on time 3, COMMAND on time 3, `state DEGRADED`, and `unaccounted 0`. The code was not changed to chase the other split. Source stays on CPU 0 at the class priority, above the forward thread at 10, so the forward thread runs when a `MsgSend` blocks. On the 00:42 run with `-q 8`, signal 1 was popped while the queue was still empty, and the next eight signals sat there. On this run the first six signals were queued before that pop. Incoming BACKGROUND 7 was dropped because the queue was already full. COMMAND 8 then evicted the newest BACKGROUND still queued, signal 4. URGENT 9 evicted the one BACKGROUND left, signal 1. Signals 10 through 46 were dropped on arrival. The queue that was forwarded is 2, 3, 5, 6, 8, and 9.

```text
--- source (exit 0) ---
source: sent manifest of 46 signals
source: sent 46 signals
--- relay A ---
relay signal-relay-a: dropped signal 7 class BACKGROUND
relay signal-relay-a: dropped signal 4 class BACKGROUND
relay signal-relay-a: dropped signal 1 class BACKGROUND
relay signal-relay-a: dropped signal 10 class BACKGROUND
relay signal-relay-a: dropped signal 46 class BACKGROUND
relay signal-relay-a: sent signal 3 class URGENT; still waiting: signal 2 COMMAND, signal 5 COMMAND, signal 6 URGENT, signal 8 COMMAND, signal 9 URGENT
relay signal-relay-a: sent signal 8 class COMMAND; still waiting: none
relay signal-relay-a: forwarded manifest of 46 signals
--- sink (exit 0) ---
sink: state DEGRADED
sink: summary URGENT received 3, on time 3, late 0, lost 0, dropped 0
sink: summary COMMAND received 3, on time 3, late 0, lost 0, dropped 0
sink: summary BACKGROUND received 0, on time 0, late 0, lost 0, dropped 40
sink: unaccounted 0
```

Signals 11 through 45 are the same `class BACKGROUND` drop line. They are omitted here. The full log has one line each.

## 2026-11-21 18:19:39 +06 · flood, queue 4, one run, not a measurement

`make run-flood FLOOD_QUEUE=4` from `<project-dir>`. `date` printed `2026-11-21 18:19:39 +0600` before the command and `2026-11-21 18:19:44 +0600` after it. Exit code 0. Same guest and the same pin. Relay A used `-q 4`. Source exited 0 after sending all 46 signals and the manifest.

Relay A dropped signal 4 and signal 7 as `class BACKGROUND`, then `dropped signal 8 class COMMAND` and `dropped signal 5 class COMMAND`, then signals 10 through 46 as `class BACKGROUND`. No dropped line says URGENT. The forward thread popped signal 1 while nothing else was waiting (`still waiting: none` is taken at the pop, before `MsgSend` returns). The drop lines are printed during that `MsgSend`. After it, the queue that remains is 3, 2, 6, and 9. COMMAND 8 found no BACKGROUND and was dropped. URGENT 9 then evicted the newest COMMAND, signal 5.

Sink: URGENT on time 3. COMMAND received 1, dropped 2, lost 0, so the three COMMAND signals are accounted. BACKGROUND received 1, dropped 39, lost 0, so the 40 BACKGROUND signals are accounted. `sink: state DEGRADED`. `sink: unaccounted 0`.

```text
--- source (exit 0) ---
source: sent manifest of 46 signals
source: sent 46 signals
--- relay A ---
relay signal-relay-a: dropped signal 4 class BACKGROUND
relay signal-relay-a: dropped signal 7 class BACKGROUND
relay signal-relay-a: dropped signal 8 class COMMAND
relay signal-relay-a: dropped signal 5 class COMMAND
relay signal-relay-a: dropped signal 10 class BACKGROUND
relay signal-relay-a: dropped signal 46 class BACKGROUND
relay signal-relay-a: sent signal 1 class BACKGROUND; still waiting: none
relay signal-relay-a: sent signal 3 class URGENT; still waiting: signal 2 COMMAND, signal 6 URGENT, signal 9 URGENT
relay signal-relay-a: sent signal 2 class COMMAND; still waiting: none
relay signal-relay-a: forwarded manifest of 46 signals
--- sink (exit 0) ---
sink: state DEGRADED
sink: summary URGENT received 3, on time 3, late 0, lost 0, dropped 0
sink: summary COMMAND received 1, on time 1, late 0, lost 0, dropped 2
sink: summary BACKGROUND received 1, on time 1, late 0, lost 0, dropped 39
sink: unaccounted 0
```

Signals 11 through 45 are omitted as in the queue 6 section.

The same session's `make run-flood` with the default queue of 8 exited 0 and did not repeat the 00:42 totals. Relay A dropped signal 7 and then 10 through 46, all `class BACKGROUND` (38 lines). Sink: URGENT on time 3, COMMAND on time 3, BACKGROUND received 2, dropped 38, lost 0, `unaccounted 0`. The first forward line still has signal 1 queued, so URGENT 9 evicted BACKGROUND 7. The 00:42 run had already popped signal 1, so nothing in that queue of 8 needed to be evicted. `make run` then printed 9 signals on time and `unaccounted 0`. `make run-kill` and `make run-lock` still printed the SAFE, restart, NORMAL, lost 6–18, LOCKED_SAFE, and missing-manifest lines.

## Baseline latency, current relay

`make measure` from `<project-dir>`. `date` printed `2026-11-21 19:29:11 +0600` before the command and `2026-11-21 19:29:48 +0600` after it. Exit code 0. N is 34: 34 calm logs and 34 flood logs, 68 files, each containing `sink: unaccounted 0`. The host report printed `latency unparsed 0` for each scenario. URGENT n is 102 in both scenarios, so the nearest-rank 99th percentile is printed. BACKGROUND under flood has n 84, so its 99th percentile is `n/a`.

This is not a threshold and the 1 second signal deadline was not changed. The relay code was not changed. The CPU 0 pin (D-036) is a condition of these numbers.

Conditions, read in this session, not from an earlier note:

- Guest, from `uname -a` on the same ssh port before the runs, and again from `make run-kill`: `QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le`. `pidin info` showed two processors.
- The running QEMU process was `qemu-system-aarch64 -machine virt -cpu cortex-a57 -smp 2 -accel tcg -m 1G`.
- Mac model from `sysctl -n hw.model`: `MacBookAir10,1`.
- Calm is the `make run` configuration: 3 URGENT, 3 COMMAND, 3 BACKGROUND, relay A queue left at its default. Flood is the `make run-flood` configuration: 3, 3, and 40, relay A queue 8.

`sink` prints three hop segments. The second is A→B. The relay stamps a signal when it receives it, so time spent in relay A's queue is inside A→B. The first segment is issue to relay A. The third is relay B to sink. The table below is the full latency and A→B. URGENT's other two segments are in the paragraph under the table, because that is what the priority hypothesis talks about.

| Class | Scenario | n | total median ns | total p99 ns | total max ns | A→B median ns | A→B p99 ns | A→B max ns |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| URGENT | calm | 102 | 3527008 | 54946000 | 55128992 | 2373504 | 6058000 | 8658000 |
| URGENT | flood | 102 | 16741496 | 21396000 | 21518000 | 15600008 | 20699008 | 20945008 |
| COMMAND | calm | 102 | 5919000 | 56667008 | 56878992 | 4872000 | 12032992 | 14420000 |
| COMMAND | flood | 102 | 19187008 | 24148000 | 24712000 | 18187000 | 23146000 | 23372000 |
| BACKGROUND | calm | 102 | 7646496 | 58452992 | 58687008 | 6597000 | 16296000 | 16522992 |
| BACKGROUND | flood | 84 | 20860496 | n/a | 25476000 | 20198504 | n/a | 24568000 |

URGENT hop 1 (issue to relay A), median / p99 / max: calm 48992 / 120000 / 122000 ns; flood 60000 / 461008 / 463008 ns. URGENT hop 3 (relay B to sink): calm 977504 / 53856000 / 54012992 ns; flood 859000 / 1464992 / 1482000 ns.

The median move matches a wait in relay A's queue. Flood's URGENT total median is 13214488 ns above calm's. Flood's A→B median is 13226504 ns above calm's. Those two gaps differ by 12016 ns. Hop 1's median stays near 50 µs, and hop 3's median is lower under flood, not higher. Every one of the 102 flood URGENT A→B samples is at least 10 ms. None of the 102 calm URGENT A→B samples is.

The maximum does not match "flood is when URGENT waits the longest." Calm's URGENT total maximum, 55128992 ns, is above flood's maximum, 21518000 ns. The calm 99th percentile, 54946000 ns, is likewise above flood's 21396000 ns. Three calm URGENT signals sit on that tail, and the time is hop 3, not A→B: hops 42000 + 857008 + 54012992 (total 54912000), hops 44992 + 1228000 + 53856000 (total 55128992), and hops 44000 + 1660000 + 53242000 (total 54946000). Flood has no URGENT hop 3 at or above 2 ms. Flood's own maximum is on A→B (20945008 ns), and it is still below those three calm totals.

Every hop and total printed in the 68 logs, 2376 numbers, ends in 000, 008, or 992. No other residue modulo 1000 appears. The unit on the line is nanoseconds. The values are coarser than 1 ns. That limit is F-022. The table keeps the printed integers.

Separate from `make measure`, this session ran `make run` and `make run-flood` once each. Both exited 0 with `sink: unaccounted 0`. `make run` printed 9 signals on time, signal 1 `on time, held`, and no dropped line. `make run-flood` dropped 37 signals, all `class BACKGROUND`, and sink printed URGENT on time 3, COMMAND on time 3, BACKGROUND received 3, dropped 37. That is the other drop count already seen for queue 8, not a new shape. `make run-kill` exited 0 and printed `sink: state SAFE`, `supervisor: restarted relay-b`, `sink: state NORMAL` again, `lost signal` 6 through 18, and `sink: unaccounted 0`. `make run-lock` exited 0, restarted relay B once, printed `supervisor: LOCKED_SAFE` and `sink: state LOCKED_SAFE`, did not start relay B a third time, and sink printed that the manifest did not arrive.

## Relay forward thread inherits its queue priority

`make measure` from `<project-dir>`, N=34. `date` printed `2026-11-21 21:51:54 +0600` before the command and `2026-11-21 21:52:32 +0600` after it. Exit code 0. 34 calm logs and 34 flood logs, 68 files, each containing `sink: unaccounted 0`. The host report printed `latency unparsed 0` for each scenario. URGENT n is 102 in both scenarios. BACKGROUND under flood has n 1144, so its 99th percentile is printed. The B10 logs in `build/measure-b10-thin/` were not modified. The same host report, run again on those logs, reproduced the B10 table below.

The forward thread now runs at the priority of the most urgent signal in its queue. The success lines were fixed before this run: (a) flood URGENT total median at most twice the calm URGENT total median from this same session; (b) under flood the total medians go URGENT, then COMMAND, then BACKGROUND; (c) no dropped URGENT or COMMAND at queue 8, and `unaccounted 0` in every run. The CPU 0 pin and the class priorities are unchanged. The 1 second deadline is unchanged.

Conditions, read in this session:

- Guest, from `uname -a` on the same ssh port, and again from `make run-kill`: `QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le`. `pidin info` showed two processors, `Processor1` and `Processor2`, both Cortex-A57.
- The running QEMU process was `qemu-system-aarch64 -machine virt -cpu cortex-a57 -smp 2 -accel tcg -m 1G`.
- Mac model from `sysctl -n hw.model`: `MacBookAir10,1`.
- Calm is the `make run` configuration: 3 URGENT, 3 COMMAND, 3 BACKGROUND, relay A queue left at its default. Flood is the `make run-flood` configuration: 3, 3, and 40, relay A queue 8.

`sink` prints three hop segments. The second is A→B. Time in relay A's queue is inside A→B. The table is full latency and A→B, B10 against this run.

| Class | Scenario | Run | n | total median ns | total p99 ns | total max ns | A→B median ns | A→B p99 ns | A→B max ns |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| URGENT | calm | B10 | 102 | 3527008 | 54946000 | 55128992 | 2373504 | 6058000 | 8658000 |
| URGENT | calm | B11 | 102 | 3531496 | 5656992 | 8290000 | 1923504 | 3527008 | 3528000 |
| URGENT | flood | B10 | 102 | 16741496 | 21396000 | 21518000 | 15600008 | 20699008 | 20945008 |
| URGENT | flood | B11 | 102 | 3571496 | 51014992 | 51112000 | 1405008 | 2916000 | 4442000 |
| COMMAND | calm | B10 | 102 | 5919000 | 56667008 | 56878992 | 4872000 | 12032992 | 14420000 |
| COMMAND | calm | B11 | 102 | 2231504 | 7258000 | 7548992 | 1343008 | 3840992 | 4026000 |
| COMMAND | flood | B10 | 102 | 19187008 | 24148000 | 24712000 | 18187000 | 23146000 | 23372000 |
| COMMAND | flood | B11 | 102 | 2219504 | 53213008 | 54646992 | 1222504 | 2396000 | 3700000 |
| BACKGROUND | calm | B10 | 102 | 7646496 | 58452992 | 58687008 | 6597000 | 16296000 | 16522992 |
| BACKGROUND | calm | B11 | 102 | 4501504 | 9318000 | 9455008 | 2279504 | 5246000 | 5455008 |
| BACKGROUND | flood | B10 | 84 | 20860496 | n/a | 25476000 | 20198504 | n/a | 24568000 |
| BACKGROUND | flood | B11 | 1144 | 7668000 | 55028000 | 56994992 | 5910000 | 11999008 | 17996000 |

(a) met. Flood URGENT total median is 3571496 ns. Calm URGENT total median in this session is 3531496 ns. Twice that is 7062992 ns. 3571496 is below 7062992.

(b) not met. Under flood the total medians are URGENT 3571496 ns, COMMAND 2219504 ns, BACKGROUND 7668000 ns. COMMAND is below URGENT, so the order is not URGENT, then COMMAND, then BACKGROUND. A→B medians under flood are URGENT 1405008 ns, COMMAND 1222504 ns, BACKGROUND 5910000 ns, the same inversion on the middle hop. The criterion was not changed. A pool of forward threads is left for a later batch.

(c) met. The 34 flood logs contain 216 `dropped signal` lines, every one `class BACKGROUND`. No dropped URGENT and no dropped COMMAND. Sink summaries in those logs show dropped 0 for URGENT and for COMMAND. All 68 logs contain `sink: unaccounted 0`. Every flood log prints `relay signal-relay-a: boosts N` with N of 9, 10, or 11.

The B10 calm stall on hop 3 happened again, in `build/measure/flood-22.log`. All 46 signals in that log have hop 3 from 49066000 ns to 55266992 ns. Their A→B hops stay near 1 ms, so the time is relay B to sink, not relay A's queue. One URGENT line is hops 77008 + 1026992 + 50008000, total 51112000. That single run is why flood p99 and maximum sit near 51 ms while the medians do not. No other measure log has a hop 3 at or above 20 ms. The separate `make run` after the measure showed the same shape on all 9 signals: hop 3 from 46038992 ns to 51074000 ns, totals from 46984992 ns to 53318000 ns, A→B still near 1 ms. Those nine were on time because the deadline is 1 second.

Every hop and total printed in the 68 logs, 6616 numbers, ends in 000, 008, or 992. No other residue modulo 1000 appears. That is the same limit as F-022.

Separate from `make measure`, this session ran the regression after the code change. `make run-flood` exited 0, source exit 0, dropped signals 10 through 46, all `class BACKGROUND` (37 lines), URGENT on time 3, COMMAND on time 3, BACKGROUND received 3, dropped 37, `unaccounted 0`. `make run-flood FLOOD_QUEUE=4` exited 0, source exit 0, 9 dropped lines, all `class BACKGROUND`, URGENT and COMMAND dropped 0, BACKGROUND received 31, `unaccounted 0`. The queue 4 and queue 6 drop tables from the earlier batch no longer apply, because the forward thread drains the queue during the burst. `make run` exited 0 with 9 signals on time, dropped 0, lost 0, `unaccounted 0`, and the hop 3 stall described above. `make run-kill` exited 0 and printed `sink: state SAFE`, `supervisor: restarted relay-b`, `sink: state NORMAL` again, `lost signal` 6 through 18, and `sink: unaccounted 0`. `make run-lock` exited 0, restarted relay B once, printed `supervisor: LOCKED_SAFE` and `sink: state LOCKED_SAFE`, did not start relay B a third time, and sink printed that the manifest did not arrive.

## R5 and the wait behind a send

`make measure` from `<project-dir>`, N=34, seeds equal to the run numbers. `date` printed `2026-11-21 22:47:18 +0600` before the command and `2026-11-21 22:47:57 +0600` after it. Exit code 0. 34 calm logs and 34 flood logs. Each log contains `sink: unaccounted 0`. The host report printed `latency unparsed 0` and `relay unparsed 0` for each scenario. URGENT n is 102 in both scenarios. Sink summaries give URGENT received 3, on time 3, dropped 0 in every log, and the same for COMMAND. BACKGROUND dropped 0 in every calm log. Flood sink summaries add to 476 dropped BACKGROUND and 0 dropped URGENT or COMMAND.

Each relay send line now ends with `arrived`, `start`, and `end`. `arrived` is that relay's own hop stamp. `start` is `CLOCK_MONOTONIC` immediately before `MsgSend`. `end` is the same clock immediately after. The host report counts two things on each relay, inside one log, then sums across the logs of a scenario.

An R5 violation is a less urgent signal L whose `MsgSend` starts while a more urgent signal U is already on that relay and has not started: `arrive(U) < start(L) < start(U)`. A wait behind a send is a signal U that arrives during some other signal's send: `start(L) < arrive(U) < end(L)`. The wait is `start(U) - arrive(U)`. The report prints every violation with the log name and both signal numbers. One flood line is:

`relay signal-relay-a R5 violation build/measure/flood-01.log signal 4 class COMMAND start 262474713215008; signal 5 class URGENT arrived 262474712712000 start 262474714627008`

There `262474712712000 < 262474713215008 < 262474714627008`.

(r) is unmet. Violation counts, sink still free to leave CPU 0:

| Scenario | relay A | relay B |
| --- | ---: | ---: |
| calm | 76 | 88 |
| flood | 64 | 105 |

Waits behind a send on that run:

| Scenario | Relay | Class | n | median ns | p99 ns | max ns |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| calm | A | URGENT | 25 | 1240992 | n/a | 3618000 |
| calm | A | COMMAND | 20 | 1054992 | n/a | 2623008 |
| calm | A | BACKGROUND | 6 | 3380504 | n/a | 5870992 |
| calm | B | URGENT | 7 | 564000 | n/a | 3471008 |
| calm | B | COMMAND | 0 |  |  |  |
| calm | B | BACKGROUND | 9 | 1166992 | n/a | 2123008 |
| flood | A | URGENT | 24 | 1110000 | n/a | 14172000 |
| flood | A | COMMAND | 18 | 1062504 | n/a | 14932000 |
| flood | A | BACKGROUND | 681 | 6608000 | 15164000 | 15680000 |
| flood | B | URGENT | 9 | 570992 | n/a | 1946992 |
| flood | B | COMMAND | 0 |  |  |  |
| flood | B | BACKGROUND | 11 | 1222000 | n/a | 4356000 |

The same measure, after the sink default became a pin to CPU 0, ran from `2026-11-21 22:50:55 +0600` to `2026-11-21 22:51:32 +0600`. Exit code 0. Again 68 logs, `unaccounted 0` in each, `latency unparsed 0`, `relay unparsed 0`, URGENT n 102 in both scenarios. (r) is unmet again: calm relay A 68 and relay B 77; flood relay A 64 and relay B 106. The report printed every violation. Waits on that second run: calm relay A URGENT n 24 median 984504 ns max 3332992 ns, COMMAND n 18 median 1020000 ns max 2222000 ns, BACKGROUND n 5 median 3102992 ns max 5330992 ns; calm relay B URGENT n 8 median 720496 ns max 3206992 ns, COMMAND n 0, BACKGROUND n 8 median 1022000 ns max 2198992 ns; flood relay A URGENT n 23 median 1056000 ns max 2306000 ns, COMMAND n 17 median 990000 ns max 2194992 ns, BACKGROUND n 703 median 6734000 ns p99 10790992 ns max 13994000 ns; flood relay B URGENT n 9 median 672992 ns max 2234992 ns, COMMAND n 0, BACKGROUND n 13 median 997008 ns max 1818992 ns.

## Shuffled issue order, full latency

The class-order line was fixed before either measure. Under flood, with each complete triple shuffled, the full-latency medians go strictly URGENT, then COMMAND, then BACKGROUND.

`make run` and `make run-flood` do not pass a seed. Their source log is still background, command, urgent. The seeded calm-01 log starts urgent, command, background.

Guest, from `uname -a` on the same ssh port in this session: `QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le`.

| Class | Scenario | Sink | n | total median ns | total p99 ns | total max ns |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| URGENT | calm | free | 102 | 2693000 | 6296000 | 6557008 |
| URGENT | flood | free | 102 | 3046000 | 55083008 | 55262000 |
| COMMAND | calm | free | 102 | 3365992 | 6725008 | 7286000 |
| COMMAND | flood | free | 102 | 3391496 | 57978000 | 58874992 |
| BACKGROUND | calm | free | 102 | 5162496 | 8740000 | 9733008 |
| BACKGROUND | flood | free | 884 | 7957000 | 20702000 | 60334000 |
| URGENT | calm | pinned | 102 | 2526504 | 49778000 | 51029008 |
| URGENT | flood | pinned | 102 | 3065504 | 18546000 | 19535008 |
| COMMAND | calm | pinned | 102 | 3543992 | 51704000 | 52940992 |
| COMMAND | flood | pinned | 102 | 3376000 | 19590000 | 19918000 |
| BACKGROUND | calm | pinned | 102 | 5182496 | 52206000 | 53378992 |
| BACKGROUND | flood | pinned | 915 | 8024000 | 18476992 | 21961008 |

(b') met on the free-sink flood row: 3046000 < 3391496 < 7957000. (b') met again on the pinned-sink flood row: 3065504 < 3376000 < 8024000. The criterion was not changed between the runs. The free-sink measure printed `stall runs 0` for both scenarios. The pinned-sink calm scenario printed one stall, `build/measure/calm-31.log`, hop-3 median 50280000 ns (9 signals, hop 3 from 48224000 ns to 51386000 ns). Its flood scenario printed `stall runs 0`.

## Sink stall experiment

A stall run is a run whose median relay-B-to-sink hop, across the signals in that log, is greater than 20 ms. The rule was fixed before the runs: 20 calm runs with sink free, then 20 with sink pinned to CPU 0. If the pinned group has 0 stalls and the free group has at least 1, sink pins to CPU 0 by default.

`make stall-check` from `2026-11-21 22:48:28 +0600` to `2026-11-21 22:48:52 +0600`. Exit code 0. 20 logs in `build/stall/free-*.log` and 20 in `build/stall/pinned-*.log`. The report printed:

```
=== free ===
stall runs 2
stall run build/stall/free-01.log median 56589008 ns
stall run build/stall/free-04.log median 49935008 ns
=== pinned ===
stall runs 0
```

Those two medians match the sink lines: free-01 hop 3 from 52943008 ns to 58604000 ns, median 56589008 ns; free-04 hop 3 from 46762000 ns to 52669008 ns, median 49935008 ns. The rule fired. Sink now pins its thread to CPU 0 with `ThreadCtl`. `-c 0` is still accepted and is the same pin. The follow-up measure in the previous section is that default. One later shuffled calm log, `calm-31`, still has a hop-3 median above 20 ms, so the pin removed the stalls inside this 20-and-20 experiment and did not remove every later stall.

After that pin was the default, regression on the guest exited 0. `make run` at 22:50:07 +06: source order background, command, urgent; 9 signals on time; dropped 0; lost 0; `unaccounted 0`. `make run-flood`: source exit 0; the same fixed order; dropped signals 28, 31, 36, 39, 42, and 45, all `class BACKGROUND`; URGENT on time 3; COMMAND on time 3; BACKGROUND received 34, dropped 6; `unaccounted 0`. `make run-kill`: `sink: state SAFE`, `supervisor: restarted relay-b`, `sink: state NORMAL` again, `lost signal` 6 through 18, `unaccounted 0`. `make run-lock`: one `supervisor: restarted relay-b`, then `supervisor: LOCKED_SAFE` and `sink: state LOCKED_SAFE`, no third start, and sink printed that the manifest did not arrive.

## Receive thread queues a signal before it drops priority

The receive thread on an accepted signal now replies, enqueues, then sets its own priority. The reply is still first. The priority drop is what moved. Class priorities, the forward-thread boost, the hop stamp, the relay line format, and the sink CPU 0 pin are unchanged.

`make measure` from `<project-dir>`, N=34, seed equal to the run number. `date` printed `2026-11-22 00:11:17 +0600` before the guest loops and `2026-11-22 00:11:56 +0600` after the report. Exit code 0. 34 calm logs and 34 flood logs. Each log contains `sink: unaccounted 0`. The host report printed `latency unparsed 0` and `relay unparsed 0` for each scenario. URGENT n is 102 in both scenarios. Guest, from `uname -a` on the same ssh port before this run: `QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le`. Source, both relays, and sink stay pinned to CPU 0. This is QEMU TCG, not hardware. The comparison column is the pinned-sink measure from the previous section (calm 68/77, flood 64/106, sum 315).

R5 violations, relay A / relay B. The report printed every B13 line. All 38 remaining lines are relay B. Relay A is 0 in both scenarios.

| Scenario | B12 A | B12 B | B13 A | B13 B |
| --- | ---: | ---: | ---: | ---: |
| calm | 68 | 77 | 0 | 17 |
| flood | 64 | 106 | 0 | 21 |
| sum | 132 | 183 | 0 | 38 |

The B12 sum is 315. The B13 sum is 38. The line fixed before the run was 31, which is 10% of 315. 38 is above that line, so the R5 criterion is unmet. The criterion was not changed.

Full-latency medians, nanoseconds, from the same two reports:

| Class | Scenario | B12 pinned | B13 |
| --- | --- | ---: | ---: |
| URGENT | calm | 2526504 | 1105496 |
| URGENT | flood | 3065504 | 1085000 |
| COMMAND | calm | 3543992 | 2195496 |
| COMMAND | flood | 3376000 | 2096008 |
| BACKGROUND | calm | 5182496 | 5812504 |
| BACKGROUND | flood | 8024000 | 6098000 |

Under flood the B13 medians go 1085000, then 2096008, then 6098000. That is URGENT, then COMMAND, then BACKGROUND. The flood URGENT median 1085000 is below twice the calm URGENT median from this same session, 2 × 1105496 = 2210992. Across the 34 flood logs the relay drop lines are 274 BACKGROUND and 0 URGENT or COMMAND. Calm drop lines are 0.

One calm line from the report, so the shape is visible: `relay signal-relay-b R5 violation build/measure/calm-22.log signal 1 class COMMAND start 267507464096000; signal 2 class URGENT arrived 267507464020000 start 267507465138000`. There the urgent arrival is 76000 ns before the command start.

Separate from the measure, the same binary was run through `make run`, `make run-flood`, `make run-kill`, and `make run-lock` from 00:13:03 to 00:13:35 +06. Each exited 0. `make run`: source exit 0, order background, command, urgent, 9 signals on time, dropped 0, lost 0, `unaccounted 0`. `make run-flood`: source exit 0, the same order, dropped signals 40 and 43, both `class BACKGROUND`, URGENT on time 3, COMMAND on time 3, BACKGROUND received 38, dropped 2, `unaccounted 0`. `make run-kill`: `sink: state SAFE`, `supervisor: restarted relay-b`, `sink: state NORMAL` again, `lost signal` 6 through 18, `unaccounted 0`. `make run-lock`: one `supervisor: restarted relay-b`, then `supervisor: LOCKED_SAFE` and `sink: state LOCKED_SAFE`, no third start, and sink printed that the manifest did not arrive.

## 2026-11-23 19:41:44 +06 · stop relay B, kill source, one run each, not a measurement

Guest, from `uname -a` inside both scenario scripts, before the fault: `QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le`. Open port 2222 is not that proof. The supervisor waits on a stopped-but-alive node and does not send SIGKILL. A dead source is a death: the supervisor restarts it inside K and the successor rebuilds the issued list from complete `source: sent signal` lines. The next number is max+2. A number with no printed line is not in the summary. Times below are one run on QEMU TCG, not an R6 or R7 figure. The 1 s deadline stub and the 2000 ms pulse window were not changed.

### make run-stop

Command: `make run-stop`. The log file was created at 2026-11-23 19:41:44 +06 and last written at 19:42:19 +06. Exit code 0. Relay B pid 20668440 stayed alive after SIGSTOP (`kill -0`, and `pidin` showed thread 1 `STOPPED`). There is no `supervisor: restarted relay-b`. The `signal=9` lines are the shutdown SIGKILL after the run. Sink printed SAFE, then 11 late lines and no late line marked fresh, then RECOVERING on URGENT 18 (held, total 691287008 ns), then NORMAL on URGENT 24 (fresh, total 1815008 ns). Five signals that were still waiting in relay A at continue arrived on time and held, because their age was under 1 s: 17 (898238992 ns), 18, 19 (492668992 ns), 20 (283320000 ns), 21 (74364992 ns). No on-time line has a total above 1000000000 ns. Summary: URGENT received 20, on time 16, late 4, lost 0; COMMAND received 10, on time 7, late 3, lost 0; BACKGROUND received 10, on time 6, late 4, lost 0. `sink: unaccounted 0`.

```text
mkdir -p build
docker exec qnx-build "/home/diana/Downloads/sample 4 projects/project-kit/build.sh"
QNX_HOST=/home/diana/qnx800/host/linux/x86_64
QNX_TARGET=/home/diana/qnx800/target/qnx
MAKEFLAGS=-I/home/diana/qnx800/target/qnx/usr/include
ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes root@localhost "slay -Q -f supervisor >/dev/null 2>&1 || true; slay -Q -f sink >/dev/null 2>&1 || true; slay -Q -f relay >/dev/null 2>&1 || true; slay -Q -f source >/dev/null 2>&1 || true"
scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes ./build/source ./build/sink ./build/relay ./build/supervisor root@localhost:/tmp/
scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes ./scripts/qnx-scenario.sh root@localhost:/tmp/qnx-scenario.sh
ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/supervisor /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh stop'
guest: QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
scenario: stopping relay-b pid 20668440
scenario: relay-b still alive pid 20668440
     pid tid name                         prio STATE          Blocked                     
20668440   1 tmp/relay                     30f STOPPED                                    
20668440   2 tmp/relay                     10f CONDVAR        (0x42087295e8)              
scenario: continuing relay-b pid 20668440
--- supervisor (exit 0) ---
supervisor: watching with COIDDEATH, K=3
supervisor: node sink pid=20660247
supervisor: node relay-b pid=20668440
supervisor: node relay-a pid=20668441
supervisor: node source pid=20668442
supervisor: death pulse sink coid=1073741825
supervisor: sink pulse pid=20660247 exit=0 signal=0
supervisor: source wait pid=20668442 exit=0 signal=0
supervisor: relay-b stop pid=20668440 exit=0 signal=9
supervisor: relay-a stop pid=20668441 exit=0 signal=9
--- source ---
source: SCHED_FIFO ceiling 253, sending 20 URGENT, 10 COMMAND, 10 BACKGROUND to signal-relay-a
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
source: sent signal 19 class BACKGROUND
source: sent signal 20 class COMMAND
source: sent signal 21 class URGENT
source: sent signal 22 class BACKGROUND
source: sent signal 23 class COMMAND
source: sent signal 24 class URGENT
source: sent signal 25 class BACKGROUND
source: sent signal 26 class COMMAND
source: sent signal 27 class URGENT
source: sent signal 28 class BACKGROUND
source: sent signal 29 class COMMAND
source: sent signal 30 class URGENT
source: sent signal 31 class URGENT
source: sent signal 32 class URGENT
source: sent signal 33 class URGENT
source: sent signal 34 class URGENT
source: sent signal 35 class URGENT
source: sent signal 36 class URGENT
source: sent signal 37 class URGENT
source: sent signal 38 class URGENT
source: sent signal 39 class URGENT
source: sent signal 40 class URGENT
source: sent manifest of 40 signals
source: sent 40 signals
--- relay A ---
relay signal-relay-a: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-a: sent signal 1 class BACKGROUND; still waiting: none arrived 69188527512000 start 69188528746000 end 69188529131008 picked 69188528713008
relay signal-relay-a: sent signal 2 class COMMAND; still waiting: none arrived 69188732726000 start 69188734200000 end 69188734587008 picked 69188734194000
relay signal-relay-a: sent signal 3 class URGENT; still waiting: none arrived 69188939194000 start 69188941176000 end 69188941604000 picked 69188941165008
relay signal-relay-a: sent signal 4 class BACKGROUND; still waiting: none arrived 69189146873008 start 69189148360000 end 69189148708000 picked 69189148355008
relay signal-relay-a: sent signal 5 class COMMAND; still waiting: none arrived 69189352722000 start 69189353641008 end 69189353927008 picked 69189353637008
relay signal-relay-a: sent signal 6 class URGENT; still waiting: none arrived 69189558226000 start 69189558762000 end 69192714920000 picked 69189558760000
relay signal-relay-a: sent signal 9 class URGENT; still waiting: signal 7 BACKGROUND, signal 8 COMMAND, signal 10 BACKGROUND, signal 11 COMMAND, signal 12 URGENT, signal 13 BACKGROUND, signal 14 COMMAND, signal 15 URGENT, signal 16 BACKGROUND, signal 17 COMMAND, signal 18 URGENT, signal 19 BACKGROUND, signal 20 COMMAND, signal 21 URGENT arrived 69190174464000 start 69192716180000 end 69192716538000 picked 69192716178000
relay signal-relay-a: sent signal 12 class URGENT; still waiting: signal 7 BACKGROUND, signal 8 COMMAND, signal 10 BACKGROUND, signal 11 COMMAND, signal 13 BACKGROUND, signal 14 COMMAND, signal 15 URGENT, signal 16 BACKGROUND, signal 17 COMMAND, signal 18 URGENT, signal 19 BACKGROUND, signal 20 COMMAND, signal 21 URGENT arrived 69190795189008 start 69192717199008 end 69192717820000 picked 69192717198000
relay signal-relay-a: sent signal 15 class URGENT; still waiting: signal 7 BACKGROUND, signal 8 COMMAND, signal 10 BACKGROUND, signal 11 COMMAND, signal 13 BACKGROUND, signal 14 COMMAND, signal 16 BACKGROUND, signal 17 COMMAND, signal 18 URGENT, signal 19 BACKGROUND, signal 20 COMMAND, signal 21 URGENT arrived 69191416478000 start 69192718353008 end 69192718558000 picked 69192718352000
relay signal-relay-a: sent signal 18 class URGENT; still waiting: signal 7 BACKGROUND, signal 8 COMMAND, signal 10 BACKGROUND, signal 11 COMMAND, signal 13 BACKGROUND, signal 14 COMMAND, signal 16 BACKGROUND, signal 17 COMMAND, signal 19 BACKGROUND, signal 20 COMMAND, signal 21 URGENT arrived 69192028712000 start 69192719034000 end 69192719594000 picked 69192719033008
relay signal-relay-a: sent signal 21 class URGENT; still waiting: signal 7 BACKGROUND, signal 8 COMMAND, signal 10 BACKGROUND, signal 11 COMMAND, signal 13 BACKGROUND, signal 14 COMMAND, signal 16 BACKGROUND, signal 17 COMMAND, signal 19 BACKGROUND, signal 20 COMMAND arrived 69192646706000 start 69192720134000 end 69192720524000 picked 69192720132000
relay signal-relay-a: sent signal 8 class COMMAND; still waiting: signal 7 BACKGROUND, signal 10 BACKGROUND, signal 11 COMMAND, signal 13 BACKGROUND, signal 14 COMMAND, signal 16 BACKGROUND, signal 17 COMMAND, signal 19 BACKGROUND, signal 20 COMMAND arrived 69189966005008 start 69192721293008 end 69192721409008 picked 69192721292000
relay signal-relay-a: sent signal 11 class COMMAND; still waiting: signal 7 BACKGROUND, signal 10 BACKGROUND, signal 13 BACKGROUND, signal 14 COMMAND, signal 16 BACKGROUND, signal 17 COMMAND, signal 19 BACKGROUND, signal 20 COMMAND arrived 69190589760000 start 69192721879008 end 69192722452000 picked 69192721878000
relay signal-relay-a: sent signal 14 class COMMAND; still waiting: signal 7 BACKGROUND, signal 10 BACKGROUND, signal 13 BACKGROUND, signal 16 BACKGROUND, signal 17 COMMAND, signal 19 BACKGROUND, signal 20 COMMAND arrived 69191210502000 start 69192722931008 end 69192723125008 picked 69192722930000
relay signal-relay-a: sent signal 17 class COMMAND; still waiting: signal 7 BACKGROUND, signal 10 BACKGROUND, signal 13 BACKGROUND, signal 16 BACKGROUND, signal 19 BACKGROUND, signal 20 COMMAND arrived 69191826318000 start 69192723548000 end 69192723984000 picked 69192723547008
relay signal-relay-a: sent signal 20 class COMMAND; still waiting: signal 7 BACKGROUND, signal 10 BACKGROUND, signal 13 BACKGROUND, signal 16 BACKGROUND, signal 19 BACKGROUND arrived 69192442323008 start 69192724573008 end 69192724751008 picked 69192724571008
relay signal-relay-a: sent signal 7 class BACKGROUND; still waiting: signal 10 BACKGROUND, signal 13 BACKGROUND, signal 16 BACKGROUND, signal 19 BACKGROUND arrived 69189761297008 start 69192725467008 end 69192725576000 picked 69192725466000
relay signal-relay-a: sent signal 10 class BACKGROUND; still waiting: signal 13 BACKGROUND, signal 16 BACKGROUND, signal 19 BACKGROUND arrived 69190383643008 start 69192725972000 end 69192726494000 picked 69192725971008
relay signal-relay-a: sent signal 13 class BACKGROUND; still waiting: signal 16 BACKGROUND, signal 19 BACKGROUND arrived 69191002308000 start 69192726931008 end 69192727107008 picked 69192726930000
relay signal-relay-a: sent signal 16 class BACKGROUND; still waiting: signal 19 BACKGROUND arrived 69191619537008 start 69192727494000 end 69192728039008 picked 69192727492000
relay signal-relay-a: sent signal 19 class BACKGROUND; still waiting: none arrived 69192236515008 start 69192728468000 end 69192728633008 picked 69192728467008
relay signal-relay-a: sent signal 22 class BACKGROUND; still waiting: none arrived 69192854866000 start 69192855680000 end 69192856044000 picked 69192855676000
relay signal-relay-a: sent signal 23 class COMMAND; still waiting: none arrived 69193058028000 start 69193059766000 end 69193060235008 picked 69193059759008
relay signal-relay-a: sent signal 24 class URGENT; still waiting: none arrived 69193266570000 start 69193267619008 end 69193267999008 picked 69193267614000
relay signal-relay-a: sent signal 25 class BACKGROUND; still waiting: none arrived 69193470190000 start 69193471933008 end 69193472334000 picked 69193471926000
relay signal-relay-a: sent signal 26 class COMMAND; still waiting: none arrived 69193676302000 start 69193677903008 end 69193678268000 picked 69193677896000
relay signal-relay-a: sent signal 27 class URGENT; still waiting: none arrived 69193882036000 start 69193883330000 end 69193883698000 picked 69193883324000
relay signal-relay-a: sent signal 28 class BACKGROUND; still waiting: none arrived 69194084921008 start 69194086091008 end 69194086430000 picked 69194086086000
relay signal-relay-a: sent signal 29 class COMMAND; still waiting: none arrived 69194291909008 start 69194292490000 end 69194292715008 picked 69194292486000
relay signal-relay-a: sent signal 30 class URGENT; still waiting: none arrived 69194499247008 start 69194501255008 end 69194501810000 picked 69194501247008
relay signal-relay-a: sent signal 31 class URGENT; still waiting: none arrived 69194704514000 start 69194706377008 end 69194706812000 picked 69194706369008
relay signal-relay-a: sent signal 32 class URGENT; still waiting: none arrived 69194909436000 start 69194910801008 end 69194911161008 picked 69194910795008
relay signal-relay-a: sent signal 33 class URGENT; still waiting: none arrived 69195112630000 start 69195114600000 end 69195115222000 picked 69195114591008
relay signal-relay-a: sent signal 34 class URGENT; still waiting: none arrived 69195317059008 start 69195317966000 end 69195318223008 picked 69195317962000
relay signal-relay-a: sent signal 35 class URGENT; still waiting: none arrived 69195525887008 start 69195527300000 end 69195527718000 picked 69195527292000
relay signal-relay-a: sent signal 36 class URGENT; still waiting: none arrived 69195732359008 start 69195733617008 end 69195734024000 picked 69195733610000
relay signal-relay-a: sent signal 37 class URGENT; still waiting: none arrived 69195940113008 start 69195941975008 end 69195942483008 picked 69195941967008
relay signal-relay-a: sent signal 38 class URGENT; still waiting: none arrived 69196143592000 start 69196144852000 end 69196145202000 picked 69196144846000
relay signal-relay-a: sent signal 39 class URGENT; still waiting: none arrived 69196347594000 start 69196348981008 end 69196349441008 picked 69196348973008
relay signal-relay-a: sent signal 40 class URGENT; still waiting: none arrived 69196555017008 start 69196556450000 end 69196557351008 picked 69196556443008
relay signal-relay-a: forwarded manifest of 40 signals
--- relay B ---
relay signal-relay-b: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-b: sent signal 1 class BACKGROUND; still waiting: none arrived 69188528806000 start 69188529243008 end 69188530144000 picked 69188529241008
relay signal-relay-b: sent signal 2 class COMMAND; still waiting: none arrived 69188734300000 start 69188734699008 end 69188735952000 picked 69188734697008
relay signal-relay-b: sent signal 3 class URGENT; still waiting: none arrived 69188941282000 start 69188941878000 end 69188943569008 picked 69188941874000
relay signal-relay-b: sent signal 4 class BACKGROUND; still waiting: none arrived 69189148453008 start 69189148811008 end 69189149589008 picked 69189148808000
relay signal-relay-b: sent signal 5 class COMMAND; still waiting: none arrived 69189353715008 start 69189354028000 end 69189354618000 picked 69189354026000
relay signal-relay-b: sent signal 6 class URGENT; still waiting: none arrived 69192714807008 start 69192715344000 end 69192716225008 picked 69192715341008
relay signal-relay-b: sent signal 9 class URGENT; still waiting: none arrived 69192716473008 start 69192717251008 end 69192717555008 picked 69192717250000
relay signal-relay-b: sent signal 12 class URGENT; still waiting: none arrived 69192717522000 start 69192718051008 end 69192718385008 picked 69192718050000
relay signal-relay-b: sent signal 15 class URGENT; still waiting: none arrived 69192718514000 start 69192719081008 end 69192719355008 picked 69192719080000
relay signal-relay-b: sent signal 18 class URGENT; still waiting: none arrived 69192719326000 start 69192719819008 end 69192720165008 picked 69192719817008
relay signal-relay-b: sent signal 21 class URGENT; still waiting: none arrived 69192720476000 start 69192720748000 end 69192720987008 picked 69192720747008
relay signal-relay-b: sent signal 8 class COMMAND; still waiting: none arrived 69192721366000 start 69192721926000 end 69192722219008 picked 69192721925008
relay signal-relay-b: sent signal 11 class COMMAND; still waiting: none arrived 69192722191008 start 69192722664000 end 69192722960000 picked 69192722663008
relay signal-relay-b: sent signal 14 class COMMAND; still waiting: none arrived 69192723081008 start 69192723590000 end 69192723836000 picked 69192723589008
relay signal-relay-b: sent signal 17 class COMMAND; still waiting: none arrived 69192723809008 start 69192724319008 end 69192724601008 picked 69192724318000
relay signal-relay-b: sent signal 20 class COMMAND; still waiting: none arrived 69192724713008 start 69192724974000 end 69192725186000 picked 69192724973008
relay signal-relay-b: sent signal 7 class BACKGROUND; still waiting: none arrived 69192725538000 start 69192726014000 end 69192726277008 picked 69192726013008
relay signal-relay-b: sent signal 10 class BACKGROUND; still waiting: none arrived 69192726250000 start 69192726696000 end 69192726959008 picked 69192726694000
relay signal-relay-b: sent signal 13 class BACKGROUND; still waiting: none arrived 69192727070000 start 69192727535008 end 69192727779008 picked 69192727534000
relay signal-relay-b: sent signal 16 class BACKGROUND; still waiting: none arrived 69192727754000 start 69192728238000 end 69192728496000 picked 69192728237008
relay signal-relay-b: sent signal 19 class BACKGROUND; still waiting: none arrived 69192728599008 start 69192728833008 end 69192729027008 picked 69192728832000
relay signal-relay-b: sent signal 22 class BACKGROUND; still waiting: none arrived 69192855768000 start 69192856175008 end 69192856807008 picked 69192856172000
relay signal-relay-b: sent signal 23 class COMMAND; still waiting: none arrived 69193059873008 start 69193060372000 end 69193061202000 picked 69193060368000
relay signal-relay-b: sent signal 24 class URGENT; still waiting: none arrived 69193267714000 start 69193268136000 end 69193269598000 picked 69193268132000
relay signal-relay-b: sent signal 25 class BACKGROUND; still waiting: none arrived 69193472037008 start 69193472464000 end 69193473321008 picked 69193472461008
relay signal-relay-b: sent signal 26 class COMMAND; still waiting: none arrived 69193677989008 start 69193678373008 end 69193679065008 picked 69193678371008
relay signal-relay-b: sent signal 27 class URGENT; still waiting: none arrived 69193883420000 start 69193883818000 end 69193884676000 picked 69193883815008
relay signal-relay-b: sent signal 28 class BACKGROUND; still waiting: none arrived 69194086172000 start 69194086535008 end 69194087295008 picked 69194086532000
relay signal-relay-b: sent signal 29 class COMMAND; still waiting: none arrived 69194292572000 start 69194292819008 end 69194293598000 picked 69194292817008
relay signal-relay-b: sent signal 30 class URGENT; still waiting: none arrived 69194501393008 start 69194502362000 end 69194504181008 picked 69194502356000
relay signal-relay-b: sent signal 31 class URGENT; still waiting: none arrived 69194706486000 start 69194706940000 end 69194711271008 picked 69194706937008
relay signal-relay-b: sent signal 32 class URGENT; still waiting: none arrived 69194910890000 start 69194911266000 end 69194911920000 picked 69194911264000
relay signal-relay-b: sent signal 33 class URGENT; still waiting: none arrived 69195114727008 start 69195115633008 end 69195116750000 picked 69195115627008
relay signal-relay-b: sent signal 34 class URGENT; still waiting: none arrived 69195318030000 start 69195318312000 end 69195318779008 picked 69195318310000
relay signal-relay-b: sent signal 35 class URGENT; still waiting: none arrived 69195527410000 start 69195527822000 end 69195528273008 picked 69195527820000
relay signal-relay-b: sent signal 36 class URGENT; still waiting: none arrived 69195733714000 start 69195734152000 end 69195735000000 picked 69195734149008
relay signal-relay-b: sent signal 37 class URGENT; still waiting: none arrived 69195942088000 start 69195942593008 end 69195943080000 picked 69195942591008
relay signal-relay-b: sent signal 38 class URGENT; still waiting: none arrived 69196144944000 start 69196145313008 end 69196145941008 picked 69196145311008
relay signal-relay-b: sent signal 39 class URGENT; still waiting: none arrived 69196349084000 start 69196349581008 end 69196350331008 picked 69196349577008
relay signal-relay-b: sent signal 40 class URGENT; still waiting: none arrived 69196556582000 start 69196557675008 end 69196560582000 picked 69196557669008
relay signal-relay-b: forwarded manifest of 40 signals
relay signal-relay-b: link down signal-sink coid 1073741826
--- sink ---
sink: state INIT
sink: signal 1 class BACKGROUND on time, held, hops 306000 ns, 1294000 ns, 566000 ns, total 2166000 ns
sink: signal 2 class COMMAND on time, held, hops 236000 ns, 1574000 ns, 516000 ns, total 2326000 ns
sink: signal 3 class URGENT on time, fresh, hops 290000 ns, 2088000 ns, 803008 ns, total 3181008 ns
sink: state NORMAL
sink: signal 4 class BACKGROUND on time, fresh, hops 267008 ns, 1580000 ns, 458992 ns, total 2306000 ns
sink: signal 5 class COMMAND on time, fresh, hops 150000 ns, 993008 ns, 392992 ns, total 1536000 ns
sink: state SAFE
sink: signal 6 class URGENT late, hops 72000 ns, 3156581008 ns, 582992 ns, total 3157236000 ns
sink: signal 9 class URGENT late, hops 330992 ns, 2542009008 ns, 816992 ns, total 2543156992 ns
sink: signal 12 class URGENT late, hops 215008 ns, 1922332992 ns, 564000 ns, total 1923112000 ns
sink: signal 15 class URGENT late, hops 236000 ns, 1302036000 ns, 602000 ns, total 1302874000 ns
sink: signal 18 class URGENT on time, held, hops 146000 ns, 690614000 ns, 527008 ns, total 691287008 ns
sink: state RECOVERING
sink: signal 21 class URGENT on time, held, hops 290992 ns, 73770000 ns, 304000 ns, total 74364992 ns
sink: signal 8 class COMMAND late, hops 292000 ns, 2755360992 ns, 592000 ns, total 2756244992 ns
sink: signal 11 class COMMAND late, hops 68992 ns, 2132431008 ns, 508992 ns, total 2133008992 ns
sink: signal 14 class COMMAND late, hops 216000 ns, 1512579008 ns, 540000 ns, total 1513335008 ns
sink: signal 17 class COMMAND on time, held, hops 206992 ns, 897491008 ns, 540992 ns, total 898238992 ns
sink: signal 20 class COMMAND on time, held, hops 638000 ns, 282390000 ns, 292000 ns, total 283320000 ns
sink: signal 7 class BACKGROUND late, hops 214000 ns, 2964240992 ns, 507008 ns, total 2964962000 ns
sink: signal 10 class BACKGROUND late, hops 250000 ns, 2342606992 ns, 477008 ns, total 2343334000 ns
sink: signal 13 class BACKGROUND late, hops 290000 ns, 1724762000 ns, 496000 ns, total 1725548000 ns
sink: signal 16 class BACKGROUND late, hops 87008 ns, 1108216992 ns, 515008 ns, total 1108819008 ns
sink: signal 19 class BACKGROUND on time, held, hops 322000 ns, 492084000 ns, 262992 ns, total 492668992 ns
sink: signal 22 class BACKGROUND on time, held, hops 134992 ns, 902000 ns, 508000 ns, total 1544992 ns
sink: signal 23 class COMMAND on time, held, hops 490000 ns, 1845008 ns, 610992 ns, total 2946000 ns
sink: signal 24 class URGENT on time, fresh, hops 144000 ns, 1144000 ns, 527008 ns, total 1815008 ns
sink: state NORMAL
sink: signal 25 class BACKGROUND on time, fresh, hops 262000 ns, 1847008 ns, 552992 ns, total 2662000 ns
sink: signal 26 class COMMAND on time, fresh, hops 676000 ns, 1687008 ns, 474992 ns, total 2838000 ns
sink: signal 27 class URGENT on time, fresh, hops 222000 ns, 1384000 ns, 496000 ns, total 2102000 ns
sink: signal 28 class BACKGROUND on time, fresh, hops 186000 ns, 1250992 ns, 464000 ns, total 1900992 ns
sink: signal 29 class COMMAND on time, fresh, hops 151008 ns, 662992 ns, 428000 ns, total 1242000 ns
sink: signal 30 class URGENT on time, fresh, hops 308000 ns, 2146000 ns, 1186992 ns, total 3640992 ns
sink: signal 31 class URGENT on time, fresh, hops 254000 ns, 1972000 ns, 588000 ns, total 2814000 ns
sink: signal 32 class URGENT on time, fresh, hops 222992 ns, 1454000 ns, 463008 ns, total 2140000 ns
sink: signal 33 class URGENT on time, fresh, hops 294000 ns, 2097008 ns, 1500000 ns, total 3891008 ns
sink: signal 34 class URGENT on time, fresh, hops 130000 ns, 970992 ns, 355008 ns, total 1456000 ns
sink: signal 35 class URGENT on time, fresh, hops 423008 ns, 1522992 ns, 497008 ns, total 2443008 ns
sink: signal 36 class URGENT on time, fresh, hops 267008 ns, 1354992 ns, 555008 ns, total 2177008 ns
sink: signal 37 class URGENT on time, fresh, hops 273008 ns, 1974992 ns, 604000 ns, total 2852000 ns
sink: signal 38 class URGENT on time, fresh, hops 252992 ns, 1352000 ns, 473008 ns, total 2078000 ns
sink: signal 39 class URGENT on time, fresh, hops 196992 ns, 1490000 ns, 631008 ns, total 2318000 ns
sink: signal 40 class URGENT on time, fresh, hops 243008 ns, 1564992 ns, 1389008 ns, total 3197008 ns
sink: summary URGENT received 20, on time 16, late 4, lost 0, dropped 0
sink: summary COMMAND received 10, on time 7, late 3, lost 0, dropped 0
sink: summary BACKGROUND received 10, on time 6, late 4, lost 0, dropped 0
sink: unaccounted 0
```

### make run-source

Command: `make run-source`. The log file was created at 2026-11-23 19:45:48 +06 and last written at 19:46:04 +06. Exit code 0. Source pid 21102618 was killed with signal 9 after it had printed signals 1 through 5. The supervisor printed `source issued list 5, next 7` and `restarted source`. Signal 6 has no printed line and does not appear at the sink. Sink printed SAFE, then RECOVERING, then NORMAL. The rebuilt sentence is present. Summary: URGENT received 13, on time 13, late 0, lost 0; COMMAND received 8, on time 8, late 0, lost 0; BACKGROUND received 8, on time 8, late 0, lost 0. `sink: unaccounted 0`.

```text
mkdir -p build
docker exec qnx-build "/home/diana/Downloads/sample 4 projects/project-kit/build.sh"
QNX_HOST=/home/diana/qnx800/host/linux/x86_64
QNX_TARGET=/home/diana/qnx800/target/qnx
MAKEFLAGS=-I/home/diana/qnx800/target/qnx/usr/include
ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes root@localhost "slay -Q -f supervisor >/dev/null 2>&1 || true; slay -Q -f sink >/dev/null 2>&1 || true; slay -Q -f relay >/dev/null 2>&1 || true; slay -Q -f source >/dev/null 2>&1 || true"
scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes ./build/source ./build/sink ./build/relay ./build/supervisor root@localhost:/tmp/
scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes ./scripts/qnx-scenario.sh root@localhost:/tmp/qnx-scenario.sh
ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/supervisor /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh source'
guest: QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
scenario: killing source pid 21102618
--- supervisor (exit 0) ---
supervisor: watching with COIDDEATH, K=3
supervisor: node sink pid=21094423
supervisor: node relay-b pid=21102616
supervisor: node relay-a pid=21102617
supervisor: node source pid=21102618
supervisor: source wait pid=21102618 exit=0 signal=9
supervisor: source streak=1 limit=3
supervisor: source issued list 5, next 7
supervisor: holding restart of source for 2500 ms
supervisor: restarted source pid=21168150
supervisor: death pulse sink coid=1073741825
supervisor: sink pulse pid=21094423 exit=0 signal=0
supervisor: source progress, streak cleared
supervisor: source wait pid=21168150 exit=0 signal=0
supervisor: relay-b stop pid=21102616 exit=0 signal=9
supervisor: relay-a stop pid=21102617 exit=0 signal=9
--- source ---
source: SCHED_FIFO ceiling 253, sending 12 URGENT, 6 COMMAND, 6 BACKGROUND to signal-relay-a
source: sent signal 1 class BACKGROUND
source: sent signal 2 class COMMAND
source: sent signal 3 class URGENT
source: sent signal 4 class BACKGROUND
source: sent signal 5 class COMMAND
source: SCHED_FIFO ceiling 253, sending 12 URGENT, 6 COMMAND, 6 BACKGROUND to signal-relay-a
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
source: sent signal 19 class BACKGROUND
source: sent signal 20 class COMMAND
source: sent signal 21 class URGENT
source: sent signal 22 class BACKGROUND
source: sent signal 23 class COMMAND
source: sent signal 24 class URGENT
source: sent signal 25 class URGENT
source: sent signal 26 class URGENT
source: sent signal 27 class URGENT
source: sent signal 28 class URGENT
source: sent signal 29 class URGENT
source: sent signal 30 class URGENT
source: sent manifest of 29 signals
source: sent 24 signals
--- relay A ---
relay signal-relay-a: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-a: sent signal 1 class BACKGROUND; still waiting: none arrived 69412998135008 start 69412999368000 end 69412999678000 picked 69412999335008
relay signal-relay-a: sent signal 2 class COMMAND; still waiting: none arrived 69413201927008 start 69413202943008 end 69413203272000 picked 69413202937008
relay signal-relay-a: sent signal 3 class URGENT; still waiting: none arrived 69413405698000 start 69413408344000 end 69413408810000 picked 69413408335008
relay signal-relay-a: sent signal 4 class BACKGROUND; still waiting: none arrived 69413610291008 start 69413611281008 end 69413611591008 picked 69413611276000
relay signal-relay-a: sent signal 5 class COMMAND; still waiting: none arrived 69413813503008 start 69413814454000 end 69413814796000 picked 69413814447008
relay signal-relay-a: sent signal 7 class BACKGROUND; still waiting: none arrived 69416781177008 start 69416783022000 end 69416783683008 picked 69416783014000
relay signal-relay-a: sent signal 8 class COMMAND; still waiting: none arrived 69416986173008 start 69416988222000 end 69416988706000 picked 69416988214000
relay signal-relay-a: sent signal 9 class URGENT; still waiting: none arrived 69417194108000 start 69417195330000 end 69417195655008 picked 69417195323008
relay signal-relay-a: sent signal 10 class BACKGROUND; still waiting: none arrived 69417401257008 start 69417403682000 end 69417404203008 picked 69417403673008
relay signal-relay-a: sent signal 11 class COMMAND; still waiting: none arrived 69417608521008 start 69417610472000 end 69417610896000 picked 69417610466000
relay signal-relay-a: sent signal 12 class URGENT; still waiting: none arrived 69417816273008 start 69417817389008 end 69417817783008 picked 69417817384000
relay signal-relay-a: sent signal 13 class BACKGROUND; still waiting: none arrived 69418021787008 start 69418024921008 end 69418025357008 picked 69418024912000
relay signal-relay-a: sent signal 14 class COMMAND; still waiting: none arrived 69418228379008 start 69418230496000 end 69418231006000 picked 69418230489008
relay signal-relay-a: sent signal 15 class URGENT; still waiting: none arrived 69418433991008 start 69418436505008 end 69418437218000 picked 69418436494000
relay signal-relay-a: sent signal 16 class BACKGROUND; still waiting: none arrived 69418642219008 start 69418644101008 end 69418644524000 picked 69418644094000
relay signal-relay-a: sent signal 17 class COMMAND; still waiting: none arrived 69418851567008 start 69418853297008 end 69418853728000 picked 69418853291008
relay signal-relay-a: sent signal 18 class URGENT; still waiting: none arrived 69419058280000 start 69419059821008 end 69419060450000 picked 69419059814000
relay signal-relay-a: sent signal 19 class BACKGROUND; still waiting: none arrived 69419266924000 start 69419268877008 end 69419269424000 picked 69419268869008
relay signal-relay-a: sent signal 20 class COMMAND; still waiting: none arrived 69419471461008 start 69419473946000 end 69419474718000 picked 69419473936000
relay signal-relay-a: sent signal 21 class URGENT; still waiting: none arrived 69419680814000 start 69419682681008 end 69419683190000 picked 69419682672000
relay signal-relay-a: sent signal 22 class BACKGROUND; still waiting: none arrived 69419888613008 start 69419889716000 end 69419890024000 picked 69419889712000
relay signal-relay-a: sent signal 23 class COMMAND; still waiting: none arrived 69420092384000 start 69420093969008 end 69420094374000 picked 69420093960000
relay signal-relay-a: sent signal 24 class URGENT; still waiting: none arrived 69420297995008 start 69420299898000 end 69420300419008 picked 69420299890000
relay signal-relay-a: sent signal 25 class URGENT; still waiting: none arrived 69420506193008 start 69420510094000 end 69420510684000 picked 69420510081008
relay signal-relay-a: sent signal 26 class URGENT; still waiting: none arrived 69420717876000 start 69420719263008 end 69420719672000 picked 69420719257008
relay signal-relay-a: sent signal 27 class URGENT; still waiting: none arrived 69420925625008 start 69420926922000 end 69420927641008 picked 69420926915008
relay signal-relay-a: sent signal 28 class URGENT; still waiting: none arrived 69421132862000 start 69421135250000 end 69421135947008 picked 69421135239008
relay signal-relay-a: sent signal 29 class URGENT; still waiting: none arrived 69421343108000 start 69421345225008 end 69421345696000 picked 69421345218000
relay signal-relay-a: sent signal 30 class URGENT; still waiting: none arrived 69421550409008 start 69421552722000 end 69421553085008 picked 69421552713008
relay signal-relay-a: forwarded manifest of 29 signals
--- relay B ---
relay signal-relay-b: SCHED_FIFO ceiling 253, own 10, BACKGROUND 12, COMMAND 20, URGENT 30
relay signal-relay-b: sent signal 1 class BACKGROUND; still waiting: none arrived 69412999424000 start 69412999782000 end 69413000649008 picked 69412999781008
relay signal-relay-b: sent signal 2 class COMMAND; still waiting: none arrived 69413203060000 start 69413203438000 end 69413205168000 picked 69413203434000
relay signal-relay-b: sent signal 3 class URGENT; still waiting: none arrived 69413408444000 start 69413409102000 end 69413409812000 picked 69413409100000
relay signal-relay-b: sent signal 4 class BACKGROUND; still waiting: none arrived 69413611358000 start 69413611700000 end 69413612264000 picked 69413611697008
relay signal-relay-b: sent signal 5 class COMMAND; still waiting: none arrived 69413814580000 start 69413814962000 end 69413816105008 picked 69413814958000
relay signal-relay-b: sent signal 7 class BACKGROUND; still waiting: none arrived 69416783199008 start 69416783814000 end 69416784865008 picked 69416783811008
relay signal-relay-b: sent signal 8 class COMMAND; still waiting: none arrived 69416988337008 start 69416988902000 end 69416990039008 picked 69416988898000
relay signal-relay-b: sent signal 9 class URGENT; still waiting: none arrived 69417195452000 start 69417195817008 end 69417196894000 picked 69417195813008
relay signal-relay-b: sent signal 10 class BACKGROUND; still waiting: none arrived 69417403870000 start 69417404316000 end 69417405019008 picked 69417404314000
relay signal-relay-b: sent signal 11 class COMMAND; still waiting: none arrived 69417610590000 start 69417611032000 end 69417611921008 picked 69417611029008
relay signal-relay-b: sent signal 12 class URGENT; still waiting: none arrived 69417817483008 start 69417817920000 end 69417818995008 picked 69417817917008
relay signal-relay-b: sent signal 13 class BACKGROUND; still waiting: none arrived 69418025020000 start 69418025464000 end 69418026345008 picked 69418025461008
relay signal-relay-b: sent signal 14 class COMMAND; still waiting: none arrived 69418230624000 start 69418231168000 end 69418232027008 picked 69418231164000
relay signal-relay-b: sent signal 15 class URGENT; still waiting: none arrived 69418436672000 start 69418437475008 end 69418439338000 picked 69418437469008
relay signal-relay-b: sent signal 16 class BACKGROUND; still waiting: none arrived 69418644218000 start 69418644656000 end 69418645431008 picked 69418644653008
relay signal-relay-b: sent signal 17 class COMMAND; still waiting: none arrived 69418853405008 start 69418853857008 end 69418854451008 picked 69418853854000
relay signal-relay-b: sent signal 18 class URGENT; still waiting: none arrived 69419059959008 start 69419060615008 end 69419061781008 picked 69419060611008
relay signal-relay-b: sent signal 19 class BACKGROUND; still waiting: none arrived 69419269006000 start 69419269599008 end 69419270622000 picked 69419269595008
relay signal-relay-b: sent signal 20 class COMMAND; still waiting: none arrived 69419474093008 start 69419475062000 end 69419476649008 picked 69419475057008
relay signal-relay-b: sent signal 21 class URGENT; still waiting: none arrived 69419682807008 start 69419683348000 end 69419685051008 picked 69419683344000
relay signal-relay-b: sent signal 22 class BACKGROUND; still waiting: none arrived 69419889791008 start 69419890130000 end 69419890615008 picked 69419890127008
relay signal-relay-b: sent signal 23 class COMMAND; still waiting: none arrived 69420094077008 start 69420094518000 end 69420095349008 picked 69420094515008
relay signal-relay-b: sent signal 24 class URGENT; still waiting: none arrived 69420300025008 start 69420300582000 end 69420301786000 picked 69420300578000
relay signal-relay-b: sent signal 25 class URGENT; still waiting: none arrived 69420510242000 start 69420510847008 end 69420512408000 picked 69420510843008
relay signal-relay-b: sent signal 26 class URGENT; still waiting: none arrived 69420719365008 start 69420719797008 end 69420720584000 picked 69420719794000
relay signal-relay-b: sent signal 27 class URGENT; still waiting: none arrived 69420927023008 start 69420927836000 end 69420928449008 picked 69420927832000
relay signal-relay-b: sent signal 28 class URGENT; still waiting: none arrived 69421135428000 start 69421136165008 end 69421137682000 picked 69421136160000
relay signal-relay-b: sent signal 29 class URGENT; still waiting: none arrived 69421345346000 start 69421345903008 end 69421346960000 picked 69421345899008
relay signal-relay-b: sent signal 30 class URGENT; still waiting: none arrived 69421552844000 start 69421553248000 end 69421554155008 picked 69421553244000
relay signal-relay-b: forwarded manifest of 29 signals
relay signal-relay-b: link down signal-sink coid 1073741826
--- sink ---
sink: state INIT
sink: signal 1 class BACKGROUND on time, held, hops 297008 ns, 1288992 ns, 476000 ns, total 2062000 ns
sink: signal 2 class COMMAND on time, held, hops 484000 ns, 1132992 ns, 564000 ns, total 2180992 ns
sink: signal 3 class URGENT on time, fresh, hops 218000 ns, 2746000 ns, 758000 ns, total 3722000 ns
sink: state NORMAL
sink: signal 4 class BACKGROUND on time, fresh, hops 129008 ns, 1066992 ns, 433008 ns, total 1629008 ns
sink: signal 5 class COMMAND on time, fresh, hops 221008 ns, 1076992 ns, 543008 ns, total 1841008 ns
sink: state SAFE
sink: signal 7 class BACKGROUND on time, held, hops 419008 ns, 2022000 ns, 756992 ns, total 3198000 ns
sink: signal 8 class COMMAND on time, held, hops 362000 ns, 2164000 ns, 772000 ns, total 3298000 ns
sink: signal 9 class URGENT on time, held, hops 386000 ns, 1344000 ns, 523008 ns, total 2253008 ns
sink: state RECOVERING
sink: signal 10 class BACKGROUND on time, held, hops 526000 ns, 2612992 ns, 563008 ns, total 3702000 ns
sink: signal 11 class COMMAND on time, held, hops 279008 ns, 2068992 ns, 615008 ns, total 2963008 ns
sink: signal 12 class URGENT on time, held, hops 149008 ns, 1210000 ns, 598992 ns, total 1958000 ns
sink: signal 13 class BACKGROUND on time, held, hops 242000 ns, 3232992 ns, 598000 ns, total 4072992 ns
sink: signal 14 class COMMAND on time, held, hops 310000 ns, 2244992 ns, 693008 ns, total 3248000 ns
sink: signal 15 class URGENT on time, fresh, hops 376000 ns, 2680992 ns, 1042000 ns, total 4098992 ns
sink: state NORMAL
sink: signal 16 class BACKGROUND on time, fresh, hops 272000 ns, 1998992 ns, 590000 ns, total 2860992 ns
sink: signal 17 class COMMAND on time, fresh, hops 271008 ns, 1838000 ns, 588992 ns, total 2698000 ns
sink: signal 18 class URGENT on time, fresh, hops 346000 ns, 1679008 ns, 828992 ns, total 2854000 ns
sink: signal 19 class BACKGROUND on time, fresh, hops 294992 ns, 2082000 ns, 753008 ns, total 3130000 ns
sink: signal 20 class COMMAND on time, fresh, hops 348000 ns, 2632000 ns, 1242992 ns, total 4222992 ns
sink: signal 21 class URGENT on time, fresh, hops 308992 ns, 1993008 ns, 684992 ns, total 2986992 ns
sink: signal 22 class BACKGROUND on time, fresh, hops 151008 ns, 1178000 ns, 428992 ns, total 1758000 ns
sink: signal 23 class COMMAND on time, fresh, hops 218000 ns, 1693008 ns, 590000 ns, total 2501008 ns
sink: signal 24 class URGENT on time, fresh, hops 316000 ns, 2030000 ns, 704992 ns, total 3050992 ns
sink: signal 25 class URGENT on time, fresh, hops 561008 ns, 4048992 ns, 757008 ns, total 5367008 ns
sink: signal 26 class URGENT on time, fresh, hops 312992 ns, 1489008 ns, 540000 ns, total 2342000 ns
sink: signal 27 class URGENT on time, fresh, hops 194000 ns, 1398000 ns, 984000 ns, total 2576000 ns
sink: signal 28 class URGENT on time, fresh, hops 402000 ns, 2566000 ns, 966000 ns, total 3934000 ns
sink: signal 29 class URGENT on time, fresh, hops 376992 ns, 2238000 ns, 801008 ns, total 3416000 ns
sink: signal 30 class URGENT on time, fresh, hops 321008 ns, 2434992 ns, 546000 ns, total 3302000 ns
sink: an earlier source died before its issued list arrived; this list was rebuilt from the lines that source printed, and a signal with no printed line is not in this summary
sink: summary URGENT received 13, on time 13, late 0, lost 0, dropped 0
sink: summary COMMAND received 8, on time 8, late 0, lost 0, dropped 0
sink: summary BACKGROUND received 8, on time 8, late 0, lost 0, dropped 0
sink: unaccounted 0
```

### R8, this batch only

Each cell is from the run that wrote the log named in the row. Lost, late, and unaccounted are the sink summary, summed across URGENT, COMMAND, and BACKGROUND.

| Failure | Reached SAFE | Network after | lost | late | unaccounted |
| --- | --- | --- | ---: | ---: | ---: |
| Kill relay B. `make run-kill`, log mtime 2026-11-23 19:48:25 +06, exit 0 | yes, `sink: state SAFE` | NORMAL. `supervisor: restarted relay-b` | 13 (signals 6–18) | 0 | 0 |
| Stop relay B. `make run-stop`, 19:41:44–19:42:19 +06, exit 0 | yes | NORMAL. No restart of relay B | 0 | 11 | 0 |
| Kill source. `make run-source`, 19:45:48–19:46:04 +06, exit 0 | yes | NORMAL. `supervisor: restarted source`. Signal 6 was not printed and is not in the summary | 0 | 0 | 0 |
| K crashes in a row. `make run-lock`, log mtime 2026-11-23 19:48:36 +06, exit 0 | yes | LOCKED_SAFE. One restart, then lock. Sink printed that the manifest did not arrive | 0 | 0 | 0 |

The same evening, after these changes, `make run` (log mtime 19:48:06 +06) and `make run-flood` (19:48:13 +06) also exited 0. Calm: 9 signals on time, lost 0, late 0, `unaccounted 0`. Flood: dropped signals 38 and 45, both BACKGROUND; URGENT on time 3; COMMAND on time 3; BACKGROUND received 38, dropped 2; `unaccounted 0`.

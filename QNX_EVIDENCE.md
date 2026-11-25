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

## B16: `/dev/signal-source`

Host clock for this run: 2026-11-23 20:41:03–20:41:16 +06. `make run-dev` from `project-kit`, exit 0. Guest, printed by the scenario before any command:

```
guest: QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

`source live` registers `/dev/signal-source` with `resmgr_attach` and issues each class from a `CLOCK_MONOTONIC` timer pulse (`timer_create`, `SIGEV_PULSE`, `MsgRegisterEvent`). The counted burst used by `make measure` is unchanged. Commands are one line each, written with `echo` and read with `cat`.

```
scenario: path after start
nrw-rw-rw-  1 root root 0 2026-11-23 14:41 /dev/signal-source
scenario: cat after start
emission live
mode normal
flood off
urgent on
stop no
issued URGENT 1
issued COMMAND 1
issued BACKGROUND 1
error none
scenario: command flood on
scenario: cat during flood
emission live
mode flood
flood on
urgent on
stop no
issued URGENT 1
issued COMMAND 1
issued BACKGROUND 29
error none
scenario: command flood off
scenario: cat after flood off
emission live
mode normal
flood off
urgent on
stop no
issued URGENT 1
issued COMMAND 1
issued BACKGROUND 29
error none
scenario: command urgent off
scenario: cat while urgent is off
emission live
mode mute
flood off
urgent off
stop no
issued URGENT 5
issued COMMAND 3
issued BACKGROUND 33
error none
scenario: source still alive pid 26177560
scenario: command restore
scenario: cat after restore
emission live
mode normal
flood off
urgent on
stop no
issued URGENT 6
issued COMMAND 5
issued BACKGROUND 36
error none
scenario: bad command
scenario: bad command reply
scenario: cat after bad command
emission live
mode normal
flood off
urgent on
stop no
issued URGENT 8
issued COMMAND 7
issued BACKGROUND 38
error unknown command
scenario: source still alive after the bad command pid 26177560
```

The shell left the captured stderr of `echo nope` empty. The script treats a zero exit from that `echo` as a failure, and this run continued, so the write failed. The device reply is the line `error unknown command`. Source pid 26177560 was still alive, and `stop` then ended the run with source exit 0.

Relay A dropped three signals, and only those three: 20, 23, and 30, each `class BACKGROUND`. After `flood off`, the next second of the relay A log had no further `dropped signal` line. Sink:

```
sink: state SAFE
sink: signal 47 class URGENT on time, held, hops 56992 ns, 467008 ns, 196992 ns, total 720992 ns
sink: state RECOVERING
sink: signal 52 class URGENT on time, fresh, hops 236000 ns, 1568000 ns, 594000 ns, total 2398000 ns
sink: state NORMAL
sink: state DEGRADED
sink: summary URGENT received 8, on time 8, late 0, lost 0, dropped 0
sink: summary COMMAND received 7, on time 7, late 0, lost 0, dropped 0
sink: summary BACKGROUND received 35, on time 35, late 0, lost 0, dropped 3
sink: unaccounted 0
```

DEGRADED is the manifest applying those three drops. It is after NORMAL. No process was killed for the mute.

`make run-source` the same evening, 20:42:10–20:42:26 +06, exit 0, same guest line. Before the kill, `ls` printed `/dev/signal-source`. After the kill the path was gone. After `supervisor: restarted source` it was back:

```
scenario: path before kill
nrw-rw-rw-  1 root root 0 2026-11-23 14:42 /dev/signal-source
scenario: path gone after the kill
scenario: path after restart
nrw-rw-rw-  1 root root 0 2026-11-23 14:42 /dev/signal-source
```

### R8, source stops issuing URGENT

| Failure | Reached SAFE | Network after | lost | late | unaccounted |
| --- | --- | --- | ---: | ---: | ---: |
| Source stops issuing URGENT and stays alive. `make run-dev`, 2026-11-23 20:41:03–20:41:16 +06, exit 0. Source pid 26177560 was alive at SAFE and after the bad command | yes, `sink: state SAFE` | NORMAL after `restore`, then DEGRADED when the manifest counted 3 dropped BACKGROUND | 0 | 0 | 0 |

`make measure` N=34, 20:42:26–20:43:04 +06, exit 0. 68 logs, each `sink: unaccounted 0`. R5 violations: calm relay A 0, relay B 0, flood relay A 0, relay B 0, sum 0. URGENT total median under flood 701008 ns (0.701008 ms).

## 2026-11-23 22:10:45 +06 · telemetry on the guest, TCP 8000

Guest at the start of `scripts/qnx-telemetry.sh`, and again after every later run the same evening (`ssh` at 22:26 +06):

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

`qconn` on this image already listens on TCP 8000. The scenario sends it `SIGKILL` before the supervisor starts. `slay` without `-s SIGKILL` delivers `SIGTERM`, which this `qconn` ignores. The supervisor then starts `/tmp/telemetry`, which binds `0.0.0.0:8000` and UDP `127.0.0.1:8001`.

Command from `project-kit`:

```text
make run-telemetry
```

Exit code 0. The host script reads `localhost:8000` (QEMU hostfwd). One snapshot from that run, file time 22:10:45 +06:

```text
seq 13
time_ns 78103078356000
state NORMAL
source_mode normal
node source alive 1 restarts 0
node relay-a alive 1 restarts 0
node relay-b alive 1 restarts 0
node sink alive 1 restarts 0
class URGENT issued 12 on_time 12 late 0 lost 0 dropped 0 median_ns 632008 max_ns 4764000
class COMMAND issued 8 on_time 8 late 0 lost 0 dropped 0 median_ns 1079496 max_ns 3991008
class BACKGROUND issued 12 on_time 12 late 0 lost 0 dropped 0 median_ns 1323000 max_ns 9860000
end
```

The same run, from the host script and the guest supervisor log:

- First snapshot that was already NORMAL, mode normal, every node alive: seq 4, URGENT on_time 1. `cat /dev/signal-source` contained `mode normal`.
- `kill -KILL` of telemetry pid 44277781. Supervisor printed `telemetry is not a network node, K unchanged` and `supervisor: restarted telemetry pid=44515358`. The next snapshot was seq 1 of the new process, URGENT on_time 5. No `sink: state SAFE`.
- `SIGSTOP` of pid 44515358 for 1.5 s, then `SIGCONT`. Sink signal lines went from 16 to 26. URGENT late lines 0. SAFE lines 0. `kill -0` still succeeded while it was stopped. After continue, snapshot seq reached 9.
- The host closed its TCP client. A new client received seq 13, state NORMAL.
- After `echo stop` the final snapshot was seq 19, state NORMAL, mode normal. Counters matched the sink summary of that run: URGENT issued 13, on_time 13, late 0, lost 0, dropped 0; COMMAND 8, 8, 0, 0, 0; BACKGROUND 13, 13, 0, 0, 0. Guest supervisor exit 0. `sink: unaccounted 0`. No `LOCKED_SAFE`, no `restarted relay`, no `restarted source`.

## 2026-11-23 23:21 +06 · host panel, and S4 with the panel and telemetry killed

Guest before the panel runs, and again at 23:25 +06 after every later run:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

The panel is the Ruby that ships with macOS. From a checkout of this repository:

```text
/usr/bin/ruby scripts/host-panel.rb
```

It reads telemetry text from `localhost:8000` and serves the page at `127.0.0.1:8765`. No extra packages. A snapshot 2 seconds old or older is shown as stale, with its age, and the old counters are hidden.

### Screenshot during run-kill

`evidence/panel-run-kill.png` is a Quick Look render of `evidence/panel-run-kill.html`. That HTML is the page the panel froze at `GET /safe` while `make run-kill` was in SAFE (23:15 +06). The live browser capture timed out, so this file is the rendered page, not a photograph of the window. The page shows state SAFE, relay B restarts 1, source mode normal, and the six state colours.

The panel log for the same run, 23:14:29–23:14:30 +06:

```text
panel-watch current state SAFE mode normal relay-b-restarts 0 age 0
panel-watch current state SAFE mode normal relay-b-restarts 1 age 0
panel-watch current state RECOVERING mode normal relay-b-restarts 1 age 0
panel-watch current state NORMAL mode normal relay-b-restarts 1 age 0
```

During `run-dev` the same log shows `NORMAL mode flood` at 23:18:46 and `NORMAL mode mute` at 23:18:48.

### S4, two live runs of 10 seconds

Both commands were `scripts/qnx-s4.sh 10`. Each printed `s4: live seconds 10` and then `s4: stop`. Shell exit code 0 for both. `sink: unaccounted 0` in both. URGENT, COMMAND, and BACKGROUND each had late 0, lost 0, dropped 0.

| Run | URGENT on time / (on time + late + lost + dropped) | R1 | URGENT total median | URGENT total max | n |
| --- | ---: | ---: | ---: | ---: | ---: |
| Calm, panel left running | 20/20 | 1 | 2674504 ns | 4786000 ns | 20 |
| Panel process killed and telemetry sent SIGKILL 5 s after `s4: live` | 20/20 | 1 | 2452000 ns | 5358992 ns | 20 |
| Difference, fault minus calm | 0 | 0 | −222504 ns | +572992 ns | 0 |

p99 is `n/a` in both: the report needs at least 100 samples, and each run has 20. The fault log contains `supervisor: restarted telemetry pid=81137686`. COMMAND received 13 on time in both. BACKGROUND received 20 on time in both.

## 2026-11-24 00:36 +06 · one command for S1–S4

Command, from this checkout:

```text
make scenarios
```

It ran from 00:36:45 to 00:37:42 +06. Exit code 0. `scenarios failed 0`.

Guest after this run and after the later `make measure N=34`, at 00:40:52 +06:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

Table printed by that command. Every number is from this run. A cell says `порога нет` when no threshold is recorded for that requirement, and that cell is not a pass.

```text
scenario | requirement | numbers | verdict
S1 | R1 | URGENT on time 9/9 | порога нет
S1 | R3 | URGENT n 9 median 2822000 ns p99 n/a max 15233008 ns; COMMAND n 6 median 2257992 ns p99 n/a max 8442000 ns; BACKGROUND n 9 median 4136000 ns p99 n/a max 8047008 ns | порога нет
S1 | R4 | unaccounted 0; threshold 0 | прошёл
S1 | checks | path yes; URGENT 9 COMMAND 6 BACKGROUND 9; states INIT,NORMAL | прошёл
S2 | R2 share | URGENT on time flood 6/6 against S1 9/9 | порога нет
S2 | R2 median | URGENT median 2061496 ns; S1 median 2822000 ns; threshold 2x S1 5644000 ns | прошёл
S2 | R4 | unaccounted 0; dropped URGENT 0 COMMAND 0 BACKGROUND 0; threshold unaccounted 0 and URGENT 0 and COMMAND 0 | прошёл
S2 | R5 | violations 0; threshold 31 | прошёл
S2 | checks | BACKGROUND issued 46; flood signals 40; window ms 2000; duration ns 2081086000; span ns 2035935008 | прошёл
S3 | R8 kill relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 stop relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 kill source | SAFE yes; unaccounted 0 | порога нет
S3 | R8 K crashes | LOCKED_SAFE yes; unaccounted 0 | порога нет
S3 | URGENT silent | SAFE yes; source alive yes | порога нет
S3 | R6 | one run, not a measurement; kill to SAFE 2143572000 ns | порога нет
S3 | R7 | one run, not a measurement; kill to fresh URGENT 2988210992 ns | порога нет
S3 | R9 | LOCKED_SAFE yes | порога нет
S3 | checks | four faults and URGENT silent observed | прошёл
S4 | R10 | R1 S1 9/9 S4 9/9; R3 URGENT median delta -548000 ns; max delta -12739008 ns; S4 median 2274000 ns max 2494000 ns | порога нет
S4 | checks | panel and telemetry killed during a 4 s window | прошёл
scenarios failed 0
```

## 2026-11-24 00:36 +06 · one command for S1–S4

Command, from this checkout:

```text
make scenarios
```

It ran from 00:36:45 to 00:37:42 +06. Exit code 0. `scenarios failed 0`.

Guest after this run and after the later `make measure N=34`, at 00:40:52 +06:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

Table printed by that command. Every number is from this run. A cell says `порога нет` when no threshold is recorded for that requirement, and that cell is not a pass.

```text
scenario | requirement | numbers | verdict
S1 | R1 | URGENT on time 9/9 | порога нет
S1 | R3 | URGENT n 9 median 2822000 ns p99 n/a max 15233008 ns; COMMAND n 6 median 2257992 ns p99 n/a max 8442000 ns; BACKGROUND n 9 median 4136000 ns p99 n/a max 8047008 ns | порога нет
S1 | R4 | unaccounted 0; threshold 0 | прошёл
S1 | checks | path yes; URGENT 9 COMMAND 6 BACKGROUND 9; states INIT,NORMAL | прошёл
S2 | R2 share | URGENT on time flood 6/6 against S1 9/9 | порога нет
S2 | R2 median | URGENT median 2061496 ns; S1 median 2822000 ns; threshold 2x S1 5644000 ns | прошёл
S2 | R4 | unaccounted 0; dropped URGENT 0 COMMAND 0 BACKGROUND 0; threshold unaccounted 0 and URGENT 0 and COMMAND 0 | прошёл
S2 | R5 | violations 0; threshold 31 | прошёл
S2 | checks | BACKGROUND issued 46; flood signals 40; window ms 2000; duration ns 2081086000; span ns 2035935008 | прошёл
S3 | R8 kill relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 stop relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 kill source | SAFE yes; unaccounted 0 | порога нет
S3 | R8 K crashes | LOCKED_SAFE yes; unaccounted 0 | порога нет
S3 | URGENT silent | SAFE yes; source alive yes | порога нет
S3 | R6 | one run, not a measurement; kill to SAFE 2143572000 ns | порога нет
S3 | R7 | one run, not a measurement; kill to fresh URGENT 2988210992 ns | порога нет
S3 | R9 | LOCKED_SAFE yes | порога нет
S3 | checks | four faults and URGENT silent observed | прошёл
S4 | R10 | R1 S1 9/9 S4 9/9; R3 URGENT median delta -548000 ns; max delta -12739008 ns; S4 median 2274000 ns max 2494000 ns | порога нет
S4 | checks | panel and telemetry killed during a 4 s window | прошёл
scenarios failed 0
```

## 2026-11-24 01:40 +06 · class deadlines and a flood that drops

Class deadlines were set before this run, from the largest calm full latency in the B18 and B19 live windows, doubled and rounded up to 1 ms, then raised so each looser class is at least twice the previous one (D-058). Stall runs are not in that set.

| Class | Largest calm full latency | Deadline |
| --- | --- | --- |
| URGENT | 15233008 ns, S1 in B19 | 31000000 ns (31 ms) |
| COMMAND | 8442000 ns, S1 in B19 | 62000000 ns (62 ms) |
| BACKGROUND | 9860000 ns, B18 telemetry snapshot | 124000000 ns (124 ms) |

The live flood is one `SIGNAL_MSG_BATCH` of 72 BACKGROUND signals every 200 ms for the whole window. The relay queue stays 64, the same size as S1 and a calm `make measure`. 72 does not fit, so BACKGROUND is dropped. The counted `make measure` burst and its queue of 8 are unchanged.

Command, from this checkout:

```text
make scenarios
```

It ran from 01:39:20 to 01:40:24 +06. Exit code 0. `scenarios failed 0`.

Guest after this run, at 01:40:24 +06:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

Table printed by that command. Every number is from this run. A cell says `порога нет` when no threshold is recorded for that requirement, and that cell is not a pass.

```text
scenario | requirement | numbers | verdict
S1 | R1 | URGENT on time 197/197; threshold 1 and n >= 100 | прошёл
S1 | R3 | URGENT n 197 median 2214992 ns p99 24157008 ns max 27468992 ns; COMMAND n 7 median 3350000 ns p99 n/a max 12167008 ns; BACKGROUND n 11 median 2294992 ns p99 n/a max 4720000 ns | порога нет
S1 | R4 | unaccounted 0; threshold 0 | прошёл
S1 | checks | path yes; URGENT 197 COMMAND 7 BACKGROUND 11; states INIT,NORMAL | прошёл
S2 | R2 share | URGENT on time flood 159/159 against S1 197/197; threshold 1 and n >= 100 | прошёл
S2 | R2 median | URGENT median 737008 ns; S1 median 2214992 ns; threshold 2x S1 4429984 ns; p99 2644000 ns; max 2934000 ns; deadline 31000000 ns; n 159 | прошёл
S2 | R4 | unaccounted 0; dropped URGENT 0 COMMAND 0 BACKGROUND 200; threshold unaccounted 0 and URGENT 0 and COMMAND 0 | прошёл
S2 | R5 | violations 0; threshold 31 | прошёл
S2 | checks | BACKGROUND issued 1811; flood signals 1800; window ms 5000; duration ns 5045154992; span ns 4963876992; drop thirds 72/64/64 | прошёл
S3 | R8 kill relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 stop relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 kill source | SAFE yes; unaccounted 0 | порога нет
S3 | R8 K crashes | LOCKED_SAFE yes; unaccounted 0 | порога нет
S3 | URGENT silent | SAFE yes; source alive yes | порога нет
S3 | R6 | one run, not a measurement; kill to SAFE 2101946000 ns | порога нет
S3 | R7 | one run, not a measurement; kill to fresh URGENT 3192704992 ns | порога нет
S3 | R9 | LOCKED_SAFE yes | порога нет
S3 | checks | four faults and URGENT silent observed | прошёл
S4 | R10 | R1 S1 197/197 S4 197/197; R3 URGENT median delta -154992 ns; max delta -21859984 ns; S4 median 2060000 ns max 5609008 ns; threshold fraction equal, |median delta| <= 5000000 ns, |max delta| <= 31000000 ns | прошёл
S4 | checks | panel and telemetry killed during a 5 s window | прошёл
scenarios failed 0
```

S1 against S2, same run, full latency in nanoseconds.

| | S1 calm | S2 flood |
| --- | --- | --- |
| URGENT on time | 197/197 | 159/159 |
| URGENT n, median, p99, max | 197, 2214992, 24157008, 27468992 | 159, 737008, 2644000, 2934000 |
| BACKGROUND on time | 11/11 | 1611/1611 |
| BACKGROUND n, median, max | 11, 2294992, 4720000 | 1611, 52047008, 84016000 |
| BACKGROUND dropped | 0 | 200, thirds 72/64/64 |
| unaccounted | 0 | 0 |

R1, the R2 share, the R2 median, and R10 passed on the thresholds above. The BACKGROUND median and maximum under the flood are higher than in S1. Every delivered BACKGROUND was still inside the 124 ms deadline.

## 2026-11-24 01:52 +06 · S2 flood that outpaces the relay chain

The 200 ms flood above did not overload the chain: the queue of 64 drained between batches, so each batch dropped exactly 72 − 64 = 8. The flood now sends one batch of 72 BACKGROUND every 50 ms, about 1440 signals a second, while the chain forwards about 880 a second. The queue stays 64.

Two places in `source` let BACKGROUND hold back URGENT at that rate, and both are fixed:

- The timer pulse used `SIGEV_PULSE_PRIO_INHERIT`, so the timer thread woke at priority 10 and got no CPU while BACKGROUND work at 12 kept CPU 0 busy. URGENT stopped being issued and sink went to SAFE during the flood. The pulse is now created at the URGENT priority, 30.
- The `/dev/signal-source` thread took the shell's priority 10 from each message, so `flood off` waited about 4.5 s. It now runs at the COMMAND priority, 20, on its own channel with `_NTO_CHF_FIXED_PRIORITY`.

Relay A prints the queue depth when each batch arrives: `batch N queued Q dropped D waiting before W`. In this run, 1 of 88 batches found the queue empty. On average 14.2 signals were waiting, and at most 33.

Command, from this checkout:

```text
make scenarios
```

Exit code 0. Guest after the run:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

Table printed by that command. Every number is from this run.

```text
scenario | requirement | numbers | verdict
S1 | R1 | URGENT on time 195/195; threshold 1 and n >= 100 | прошёл
S1 | R3 | URGENT n 195 median 2390992 ns p99 6617008 ns max 8366992 ns; COMMAND n 7 median 4376000 ns p99 n/a max 7483008 ns; BACKGROUND n 11 median 4132000 ns p99 n/a max 11308992 ns | порога нет
S1 | R4 | unaccounted 0; threshold 0 | прошёл
S1 | checks | path yes; URGENT 195 COMMAND 7 BACKGROUND 11; states INIT,NORMAL | прошёл
S2 | R2 share | URGENT on time flood 215/215 against S1 195/195; threshold 1 and n >= 100 | прошёл
S2 | R2 median | URGENT median 1031008 ns; S1 median 2390992 ns; threshold 2x S1 4781984 ns; p99 6098000 ns; max 8318000 ns; deadline 31000000 ns; n 215 | прошёл
S2 | R4 | unaccounted 0; dropped URGENT 0 COMMAND 0 BACKGROUND 1955; threshold unaccounted 0 and URGENT 0 and COMMAND 0 | прошёл
S2 | R5 | violations 1; threshold 31 | прошёл
S2 | checks | BACKGROUND issued 6348; flood signals 6336; window ms 5000; duration ns 5033522000; span ns 5050390992; drop thirds 640/703/612 | прошёл
S3 | R8 kill relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 stop relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 kill source | SAFE yes; unaccounted 0 | порога нет
S3 | R8 K crashes | LOCKED_SAFE yes; unaccounted 0 | порога нет
S3 | URGENT silent | SAFE yes; source alive yes | порога нет
S3 | R6 | one run, not a measurement; kill to SAFE 2080546000 ns | порога нет
S3 | R7 | one run, not a measurement; kill to fresh URGENT 3194005008 ns | порога нет
S3 | R9 | LOCKED_SAFE yes | порога нет
S3 | checks | four faults and URGENT silent observed | прошёл
S4 | R10 | R1 S1 195/195 S4 198/198; R3 URGENT median delta -254488 ns; max delta -390000 ns; S4 median 2136504 ns max 7976992 ns; threshold fraction equal, |median delta| <= 5000000 ns, |max delta| <= 31000000 ns | прошёл
S4 | checks | panel and telemetry killed during a 5 s window | прошёл
scenarios failed 0
```

S1 against S2, same run, full latency in nanoseconds:

| | S1 calm | S2 flood |
| --- | --- | --- |
| URGENT on time | 195/195 | 215/215 |
| URGENT median, p99, max | 2390992, 6617008, 8366992 | 1031008, 6098000, 8318000 |
| BACKGROUND delivered, median, max | 11, 4132000, 11308992 | 4393, 49574992, 114427008 |
| BACKGROUND dropped | 0 | 1955, thirds 640/703/612 |
| unaccounted | 0 | 0 |

Under the flood, BACKGROUND waits about 12 times longer and loses 1955 signals. URGENT stays inside its 31 ms deadline, and no URGENT or COMMAND is dropped. This is one run, not a measurement series.

## 2026-11-24 16:22 +06 · panel buttons, a flood that keeps room, and send time limits

Three changes, all checked on the guest from 16:22 to 16:28 +06 with one build:

```text
7bf7188a4e99e43ad84d6883f7eafa5b7d007782  source
3e901b5837c1b584a92cc472742b16f1b2e64f33  relay
8b583e7419467fae9c5a9be3cef1d7f0edbbb896  sink
```

- **Panel buttons.** The host panel has four buttons: Flood on, Flood off, Mute URGENT, Restore URGENT. Each one is a POST to the panel server, which writes one fixed line to `/dev/signal-source` over the same ssh the Makefile uses (port 2222, batch mode). Only those four commands exist; `stop` is refused. The server listens on 127.0.0.1 only and refuses a POST without the page's own header, Origin, and Host. The mode on the page still comes from telemetry, not from the button.
- **Flood budget.** The issued list holds 12288 numbers. The flood may use all but the last 2048. When it reaches that point it turns itself off, prints one line, and `flood on` is refused for the rest of the run with `error flood budget used`. The reserve is about 38 s of the live schedule (53 signals a second). Before this, a flood longer than about 8 s filled the list and every class stopped.
- **Send time limits.** Every `MsgSend` in source now has a 5 s limit set with `TimerTimeout` on the calling thread (the manifest has 20 s). It replaces `alarm()`, which is aimed at the whole process. The flood batch had no limit at all. A batch that relay A has already received waits for a reply, and on a `name_attach` channel the limit then only asks relay A to unblock the sender. A stopped relay never answers, so source waits at most 6 s for the flood thread, then ends the run without it. The process finishes exiting when relay A dies, as the supervisor would make it.

### Commands and results

| Command | Exit | Result |
|---|---|---|
| `make test` | 0 | 732 PASS, 0 FAIL |
| `make scenarios` | 0 | `scenarios failed 0`, table below |
| `make run`, `run-flood`, `run-kill`, `run-lock`, `run-stop`, `run-source`, `run-dev`, `run-telemetry` | 0 each | `unaccounted 0` in each; `run-dev` dropped 931 |
| `make run-long-flood` | 0 | flood held 20 s, URGENT 804/804 and COMMAND 26/26 on time, `unaccounted 0` |
| `make run-flood-hang` | 0 | both variants: source gave up 25 s after relay A stopped, exit 1, 0 s after relay A died |
| `make panel-check` | 0 | four buttons, each mode seen in telemetry, `unaccounted 0` |
| `make measure` (N=34) | 0 | 68 logs, each `unaccounted 0`; R5 0; URGENT flood median 841000 ns |

### Scenarios

```text
S1 | R1 | URGENT on time 213/213; threshold 1 and n >= 100 | прошёл
S1 | R3 | URGENT n 213 median 2072992 ns p99 4423008 ns max 6634992 ns; COMMAND n 7 median 4367008 ns p99 n/a max 11350000 ns; BACKGROUND n 11 median 4656992 ns p99 n/a max 9826000 ns | порога нет
S1 | R4 | unaccounted 0; threshold 0 | прошёл
S1 | checks | path yes; URGENT 213 COMMAND 7 BACKGROUND 11; states INIT,NORMAL | прошёл
S2 | R2 share | URGENT on time flood 221/221 against S1 213/213; threshold 1 and n >= 100 | прошёл
S2 | R2 median | URGENT median 1009008 ns; S1 median 2072992 ns; threshold 2x S1 4145984 ns; p99 5006992 ns; max 6404000 ns; deadline 31000000 ns; n 221 | прошёл
S2 | R4 | unaccounted 0; dropped URGENT 0 COMMAND 0 BACKGROUND 2241; threshold unaccounted 0 and URGENT 0 and COMMAND 0 | прошёл
S2 | R5 | violations 2; threshold 31 | прошёл
S2 | checks | BACKGROUND issued 6564; flood signals 6552; window ms 5000; duration ns 5042812992; span ns 5040624992; drop thirds 751/784/706 | прошёл
S3 | R8 kill relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 stop relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 kill source | SAFE yes; unaccounted 0 | порога нет
S3 | R8 K crashes | LOCKED_SAFE yes; unaccounted 0 | порога нет
S3 | URGENT silent | SAFE yes; source alive yes | порога нет
S3 | R6 | one run, not a measurement; kill to SAFE 2149510000 ns | порога нет
S3 | R7 | one run, not a measurement; kill to fresh URGENT 3268795008 ns | порога нет
S3 | R9 | LOCKED_SAFE yes | порога нет
S3 | checks | four faults and URGENT silent observed | прошёл
S4 | R10 | R1 S1 213/213 S4 199/199; R3 URGENT median delta -12992 ns; max delta 287008 ns; S4 median 2060000 ns max 6922000 ns; threshold fraction equal, |median delta| <= 5000000 ns, |max delta| <= 31000000 ns | прошёл
S4 | checks | panel and telemetry killed during a 5 s window | прошёл
scenarios failed 0
```

### Long flood

`make run-long-flood` turns the flood on and holds it for 20 s, then reads `/dev/signal-source`, tries `flood on` again, and stops. The large logs are read only after the network stops (see the note below).

```text
long-flood: source: flood budget used at 10243 of 12288 issued numbers, 2048 kept for the live schedule; flood off (D-061)
long-flood: flood on after the budget refused; error flood budget used
long-flood: URGENT sent after the budget 484
sink: summary URGENT received 804, on time 804, late 0, lost 0, dropped 0
sink: summary COMMAND received 26, on time 26, late 0, lost 0, dropped 0
sink: summary BACKGROUND received 6957, on time 6929, late 28, lost 0, dropped 2980
sink: unaccounted 0
long-flood: PASS
```

The flood itself lasted about 7 s of the 20 s (138 batches of 72 at 50 ms); the rest of the window ran the normal schedule.

Note. In the first run of this check the script ran `awk` over the 20 000-line source log while the network was still running. Two URGENT arrived late (67084000 ns and 44376992 ns), both just before `stop`. With the reads moved after the stop, three runs had no late URGENT. The likely cause is that the nodes print every signal to files in `/tmp`, and a large read of the same files competes with them. This was not measured further.

### Relay A stops during the flood

`make run-flood-hang` stops relay A with `SIGSTOP` one second after `flood on`, first with URGENT on, then with URGENT muted.

```text
flood-hang: variant urgent
flood-hang: /dev/signal-source answered while relay A was stopped
flood-hang: source gave up 25 s after the stop
flood-hang: the flood thread was not stuck in a send
flood-hang: source exit 1, 0 s after relay A died
flood-hang: PASS urgent
flood-hang: variant mute
flood-hang: /dev/signal-source answered while relay A was stopped
source: the next node did not take a BACKGROUND batch within 5 s; the flood thread stops
flood-hang: source gave up 25 s after the stop
flood-hang: the flood thread was the one stuck
flood-hang: source exit 1, 0 s after relay A died
flood-hang: PASS mute
```

Which send gets stuck first depends on timing. An earlier run of the same command printed the third case:

```text
source: the flood thread is still waiting for the next node after 6 s; ending the run without it
```

### Panel buttons

`make panel-check` starts the live network with telemetry, starts the panel, and sends each button's POST:

```text
panel-check: buttons on the page 4
panel-check: pressed flood-on: HTTP 200, 16:27:29 sent "flood on"; the mode above comes from telemetry
panel-check: telemetry shows mode flood after flood-on
panel-check: pressed flood-off: HTTP 200, 16:27:30 sent "flood off"; the mode above comes from telemetry
panel-check: telemetry shows mode normal after flood-off
panel-check: pressed mute: HTTP 200, 16:27:30 sent "mute"; the mode above comes from telemetry
panel-check: telemetry shows mode mute after mute
panel-check: pressed restore: HTTP 200, 16:27:30 sent "restore"; the mode above comes from telemetry
panel-check: telemetry shows mode normal after restore
panel-check: POST without the panel header: HTTP 403
panel-check: stop: HTTP 400, 16:27:31 refused: unknown command stop
panel-check: guest network exit 0
panel-check: sink: summary URGENT received 80, on time 80, late 0, lost 0, dropped 0
panel-check: sink: summary COMMAND received 4, on time 4, late 0, lost 0, dropped 0
panel-check: sink: summary BACKGROUND received 288, on time 238, late 50, lost 0, dropped 365
panel-check: PASS
```

With the guest stopped (the QEMU process paused with `SIGSTOP` for 5 s at 16:21:46 and then continued), Flood on returned HTTP 502 and the page showed the error:

```text
16:21:51 "flood on" failed: Connection timed out during banner exchange
Connection to 127.0.0.1 port 2222 timed out
```

Guest after all runs:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

## 2026-11-24 18:18 +06 · a live run with no length limit

Before this change the live source kept every issued number for one manifest at the end of the run. The list held 12288 numbers, so a live run stopped issuing after about 230 s (53 signals a second), and a flood after about 7 s.

Now the books are closed while the network runs:

- Every 250 ms source sends a **close** for the numbers it issued at least 1 s ago (at most 1024 per close). Each relay passes the close on from its own thread at BACKGROUND priority, outside the signal queue, and marks the numbers it dropped. sink puts them on the books: each number has arrived, was dropped, or is counted as lost. A loss that arrives later still replaces the loss. source forgets the numbers only after sink has them; a close that fails is sent again.
- The sink ledger is a ring of 12288 slots with whole-run totals per class. A slot is reused only after its number is settled. Telemetry reads the totals instead of walking the ledger on every signal.
- The flood reserve of 2048 now counts only numbers that are not closed yet. In these runs at most 1413 were waiting, so the flood held for as long as it was on.
- With `SIGNAL_QUIET=1` the nodes skip the line per signal and keep states, closes, losses, late signals, and totals. A minute of flood with a line per signal filled the guest's `/tmp` (62 MB) in the first try of the 60 s check (`No space left on device`).
- `stall-probe` is a thread on CPU 0 at priority 31, above every signal class. It wakes every 10 ms and prints each wake-up that came late by more than the threshold, so a late signal can be checked against moments when the whole guest stopped.

One build for every result below, 2026-11-24 18:18–18:36 +06:

```text
9e72f53b6ed9edacfc89c367c6b65434fe53247d  source
9982b7209d081cb14fd1f79495c96bbda5befe82  relay
c537eccc95f8cea9803dd116635c33e904e18d5e  sink
65a63fd70853c8cb9acaaad2ad411f2f7ff906b2  stall-probe
```

### Commands and results

| Command | Exit | Result |
|---|---|---|
| `make test` | 0 | 784 PASS, 0 FAIL |
| `make scenarios` | 0 | `scenarios failed 0`, table below |
| `make run`, `run-flood`, `run-kill`, `run-lock`, `run-stop`, `run-source`, `run-dev`, `run-telemetry`, `panel-check` | 0 each | `unaccounted 0` in each; `run-dev` dropped 719 |
| `make run-flood-hang` | 0 | both variants: source gave up 25 s after relay A stopped, exit 1, 0 s after relay A died |
| `make run-long-flood` | 0 | flood held 60 s; issued per class equals the sink totals; `unaccounted 0` |
| `make measure` (N=34) | 0 | 68 logs, each `unaccounted 0`; R5 0; URGENT flood median 835000 ns |
| `make run-long-live` | 0 | 600 s, 172083 signals, `unaccounted 0`; 6 late URGENT, each inside a guest stall |

### Scenarios

```text
S1 | R1 | URGENT on time 199/199; threshold 1 and n >= 100 | прошёл
S1 | R3 | URGENT n 199 median 2247008 ns p99 7454000 ns max 9502992 ns; COMMAND n 7 median 3570000 ns p99 n/a max 6845008 ns; BACKGROUND n 11 median 4400000 ns p99 n/a max 10894000 ns | порога нет
S1 | R4 | unaccounted 0; threshold 0 | прошёл
S1 | checks | path yes; URGENT 199 COMMAND 7 BACKGROUND 11; states INIT,NORMAL | прошёл
S2 | R2 share | URGENT on time flood 223/223 against S1 199/199; threshold 1 and n >= 100 | прошёл
S2 | R2 median | URGENT median 1026992 ns; S1 median 2247008 ns; threshold 2x S1 4494016 ns; p99 4938992 ns; max 5502000 ns; deadline 31000000 ns; n 223 | прошёл
S2 | R4 | unaccounted 0; dropped URGENT 0 COMMAND 0 BACKGROUND 2313; threshold unaccounted 0 and URGENT 0 and COMMAND 0 | прошёл
S2 | R5 | violations 0; threshold 31 | прошёл
S2 | checks | BACKGROUND issued 6564; flood signals 6552; window ms 5000; duration ns 5037272000; span ns 5044618000; drop thirds 770/734/809 | прошёл
S3 | R8 kill relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 stop relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 kill source | SAFE yes; unaccounted 0 | порога нет
S3 | R8 K crashes | LOCKED_SAFE yes; unaccounted 0 | порога нет
S3 | URGENT silent | SAFE yes; source alive yes | порога нет
S3 | R6 | one run, not a measurement; kill to SAFE 2180680000 ns | порога нет
S3 | R7 | one run, not a measurement; kill to fresh URGENT 3033446000 ns | порога нет
S3 | R9 | LOCKED_SAFE yes | порога нет
S3 | checks | four faults and URGENT silent observed | прошёл
S4 | R10 | R1 S1 199/199 S4 196/196; R3 URGENT median delta -91008 ns; max delta -3113984 ns; S4 median 2156000 ns max 6389008 ns; threshold fraction equal, |median delta| <= 5000000 ns, |max delta| <= 31000000 ns | прошёл
S4 | checks | panel and telemetry killed during a 5 s window | прошёл
scenarios failed 0
```

### 60 s flood

```text
long-flood: flood on, held 60 s
long-flood: closes 236
long-flood: most numbers waiting to close 1413
long-flood: URGENT issued 2417, on the sink totals 2417
long-flood: COMMAND issued 76, on the sink totals 76
long-flood: BACKGROUND issued 76298, on the sink totals 76298
long-flood: PASS
```

The logs of the four nodes took 147 KB in all.

### Ten minutes of the live network

`make run-long-live`: flood on at 60 s and off at 120 s, mute at 240 s, restore at 250 s, flood on at 300 s and off at 360 s, stop at 600 s.

```text
long-live: ran 600 s, source exit 0, sink exit 0
long-live: closes 2318
long-live: most numbers waiting to close 1340
long-live: source: sent 172083 signals
long-live: source: issued URGENT 22839 COMMAND 745 BACKGROUND 148499
sink: summary URGENT received 22839, on time 22833, late 6, lost 0, dropped 0
sink: summary COMMAND received 745, on time 745, late 0, lost 0, dropped 0
sink: summary BACKGROUND received 131941, on time 131940, late 1, lost 0, dropped 16558
sink: unaccounted 0
long-live: late 81619 URGENT inside a guest stall of 27 ms at 151227416 ms
long-live: late 81620 URGENT inside a guest stall of 27 ms at 151227416 ms
long-live: late 82161 URGENT inside a guest stall of 60 ms at 151240430 ms
long-live: late 82503 URGENT inside a guest stall of 9 ms at 151248654 ms
long-live: late 82949 URGENT inside a guest stall of 25 ms at 151259314 ms
long-live: late 82952 URGENT inside a guest stall of 18 ms at 151259397 ms
long-live: late URGENT or COMMAND inside guest stalls 6, outside 0
long-live: PASS
```

The logs took 0.8 MB of the 62 MB. sink handled 2318 closes, on average in 140776 ns and at most 5174000 ns.

The probe saw 229 late wake-ups over 5 ms in this run, 22 of them over 20 ms, the worst 91 ms. The six late URGENT came between 199 s and 231 s, in the calm part between the two floods. "Inside a guest stall" means that the signal's way from source to sink overlaps a moment when the probe, above every class, could not run. It does not prove that the stall caused the whole delay: signal 82503 was 71 ms late and overlapped a 9 ms stall. Every late URGENT also moved sink to SAFE for a moment; the one longer SAFE is the mute at 241 s.

Three earlier 10-minute runs during this work are not counted as passes. Before the probe existed, two runs had 3 and 6 late URGENT; the second also had 1 late COMMAND. With the probe at 20 ms, a third had 9 late URGENT. 4 of them overlapped a stall over 20 ms; 5 did not, including 4 within 150 ms during the second flood. That is why the threshold went down to 5 ms. The 10-minute check passes only if every late URGENT or COMMAND overlaps a stall.

### Panel

After each button the panel now reads `mode` and `error` back from `/dev/signal-source`, so a flood that source turned off again shows its reason next to the button:

```text
panel-check: pressed flood-on: HTTP 200, 18:23:53 sent "flood on" (source: mode flood; error none); the mode above comes from telemetry
panel-check: pressed flood-off: HTTP 200, 18:23:54 sent "flood off" (source: mode normal; error none); the mode above comes from telemetry
panel-check: PASS
```

Guest after all runs:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

## 2026-11-24 20:31 +06 · killed nodes of the live network keep the books

Before this change a node killed in the live network under `supervisor -L` took its books with it (quiet logs, a flood, one kill, 60 s each):

- **source:** the restarted source numbered from 1 again. sink printed `signal N was already accounted` 45 times, then `the close repeats signal 1`, and exited with status 1. The supervisor restarted a sink nobody had killed, and the new sink started with empty books. The final summary held only the numbers issued after the restart (URGENT 1248), so `unaccounted 0` and "issued equals the sink totals" passed only because both sides had lost the start of the run. The dead source's open numbers were never closed.
- **relay A:** source got `Bad file descriptor` on its next send and exited with status 1; from there it went as above, three restarts for one kill.
- **relay B:** the books stayed whole (relay A's queue was counted lost), but 11 URGENT were late outside guest stalls.
- **sink:** the new sink started with empty books; URGENT issued 2364, on the sink totals 1274 + 128 lost. 15 URGENT were late outside guest stalls: a close with about 900 losses held the only sink thread for 111–128 ms, because each `lost signal` line was flushed on its own.

Now (decision D-063):

- The books of each node live in a named POSIX shared memory object that outlives the process: the source's ring of issued numbers, next number, and closed totals; the sink's ledger; each relay's list of dropped numbers. The supervisor starts a restarted node with `-r`, and it goes on from the dead node's books. A node started without `-r` begins new books.
- source puts a number on its book before it sends it. A number whose send did not finish stays there and is closed as lost if it did not arrive. A restarted source continues after the last number, closes the dead source's open numbers, and continues the issued totals per class.
- A kill can land between any two writes. A single entry has one commit (the count is written after the item); updates of several fields go through a redo journal that a restarted node finishes. A restarted sink counts every total again from its slots.
- sink accepts a close it has already applied (a dead source never got the answer) and keeps the accounts it gave.
- source survives the death of relay A: on `ESRCH` or `EBADF` the number stays on the book, nothing is issued until `name_open` finds the new relay A, and source then prints how much was not issued.
- sink flushes `lost signal` lines once per close.
- The panel shows a books line: how many source and sink restarts the books survived and how many issued numbers wait to be closed. A restart without books would be a red banner.

What does not survive a kill: the queue of a killed relay (its numbers are counted lost), the sink's network state and latency window (back to INIT), and the source mode (a restarted source is in normal mode, not flood or mute).

One build for every result below, 2026-11-24 20:31–21:00 +06:

```text
6fd9431db4c8289a5e147df3fbd8d6baaca0269a  source
0fa74595630fa136a32b9d129bbb3f41db957067  relay
091498f7f04df18e6a8ab16b383d52ea3bee9c75  sink
997bb3c90ddeb4d0b407edec15135ca2cf77b650  supervisor
c4aca02b8f247513eea57ae44fbdcca07c7f6976  telemetry
65a63fd70853c8cb9acaaad2ad411f2f7ff906b2  stall-probe
```

### Commands and results

| Command | Exit | Result |
|---|---|---|
| `make test` | 0 | 962 PASS, 0 FAIL |
| `make scenarios` | 2, 2, 0 | failed twice on late URGENT, passed the third time; see below |
| `make run`, `run-flood`, `run-kill`, `run-lock`, `run-stop`, `run-source`, `run-dev`, `run-telemetry`, `panel-check` | 0 each | `unaccounted 0` in each |
| `make run-flood-hang` | 0 | source gave up 26 s after relay A stopped, exit 1, 0 s after relay A died |
| `make run-long-flood` | 2, 0 | first try: one URGENT late (44 ms, 30 ms of it source to relay A); second try passed |
| `make measure` (N=34) | 0 | 68 logs, each `unaccounted 0`; R5 0; URGENT median calm 1091504 ns, flood 1003992 ns |
| `make run-long-live` | 0 | 600 s, 169288 signals, `unaccounted 0`; 13 late URGENT or COMMAND, each inside a guest stall |
| `make run-live-restart` | 0 | 300 s, four kills, below |

### Late URGENT outside the live checks

The first `make scenarios` failed on one late URGENT in S2 (61.8 ms, 60.7 ms of it between source and relay A). The second failed on S1, with no flood: one URGENT took 122.9 ms, spread over all three hops (31.5, 72.8, 18.6 ms), so S1 R1, S2 R2, and S4 R10 failed. These two checks do not run the stall probe. The third run had `stall-probe` beside it: the probe, at priority 31 above every signal class, woke more than 5 ms late 42 times in about 66 s, at worst 33 ms, and the scenarios passed. The measurement in the same build shows no slower first hop: URGENT under flood, source to relay A, median 128000 ns, maximum 304000 ns. That fits whole-guest stops, as in the live checks, where every late URGENT fell inside one; for these two runs it is not proven.

Earlier in the session, before this build, the guest kernel stopped during `make run-dev` (serial console: `Shutdown`, `kernel persona`, in `nto_time`). After a restart of QEMU the same `run-dev` passed, and every run above comes after that restart.

### Scenarios (third run)

```text
S1 | R1 | URGENT on time 196/196; threshold 1 and n >= 100 | прошёл
S1 | R3 | URGENT n 196 median 1529000 ns p99 2647008 ns max 2666000 ns; COMMAND n 7 median 1406000 ns p99 n/a max 2572000 ns; BACKGROUND n 11 median 2316992 ns p99 n/a max 5497008 ns | порога нет
S1 | R4 | unaccounted 0; threshold 0 | прошёл
S1 | checks | path yes; URGENT 196 COMMAND 7 BACKGROUND 11; states INIT,NORMAL | прошёл
S2 | R2 share | URGENT on time flood 216/216 against S1 196/196; threshold 1 and n >= 100 | прошёл
S2 | R2 median | URGENT median 1400504 ns; S1 median 1529000 ns; threshold 2x S1 3058000 ns; p99 10000992 ns; max 12732992 ns; deadline 31000000 ns; n 216 | прошёл
S2 | R4 | unaccounted 0; dropped URGENT 0 COMMAND 0 BACKGROUND 3698; threshold unaccounted 0 and URGENT 0 and COMMAND 0 | прошёл
S2 | R5 | violations 1; threshold 31 | прошёл
S2 | checks | BACKGROUND issued 6564; flood signals 6552; window ms 5000; duration ns 5047128000; span ns 5035452992; drop thirds 1216/1249/1233 | прошёл
S3 | R8 kill relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 stop relay B | SAFE yes; unaccounted 0 | порога нет
S3 | R8 kill source | SAFE yes; unaccounted 0 | порога нет
S3 | R8 K crashes | LOCKED_SAFE yes; unaccounted 0 | порога нет
S3 | URGENT silent | SAFE yes; source alive yes | порога нет
S3 | R6 | one run, not a measurement; kill to SAFE 2261652000 ns | порога нет
S3 | R7 | one run, not a measurement; kill to fresh URGENT 3331405008 ns | порога нет
S3 | R9 | LOCKED_SAFE yes | порога нет
S3 | checks | four faults and URGENT silent observed | прошёл
S4 | R10 | R1 S1 196/196 S4 203/203; R3 URGENT median delta -183000 ns; max delta 1346000 ns; S4 median 1346000 ns max 4012000 ns; threshold fraction equal, |median delta| <= 5000000 ns, |max delta| <= 31000000 ns | прошёл
S4 | checks | panel and telemetry killed during a 5 s window | прошёл
scenarios failed 0
```

### Five minutes with four kills

`make run-live-restart`: `supervisor -k 3 -w 1500 -L`, quiet logs, `stall-probe` beside it, the host panel read every 2 s. Flood on at 20 s; relay A killed at 40 s, relay B at 70 s, sink at 100 s, source at 130 s (all with the flood on); flood off at 160 s, on at 180 s, off at 210 s; mute at 230 s, restore at 240 s; stop at 300 s.

```text
supervisor: restarted relay-a pid=51687450
supervisor: restarted relay-b pid=52654104
supervisor: restarted sink pid=53809177
supervisor: restarted source pid=55136283
source: signal-relay-a is gone (signal 26144: No such process); numbers on the book are closed as lost if they did not arrive, and nothing is issued until it is back
source: reopened signal-relay-a after 2029 ms; 82 scheduled signals and 35 flood batches were not issued while it was gone
relay signal-relay-a: resumed the books of a dead relay (resume 1): 168 dropped numbers kept; its queue is lost and source closes those numbers as lost
relay signal-relay-b: resumed the books of a dead relay (resume 1): 0 dropped numbers kept; its queue is lost and source closes those numbers as lost
sink: resumed the books of a dead sink (resume 1)
sink: books kept URGENT on time 3662, late 9, lost 79, dropped 0
sink: books kept COMMAND on time 119, late 0, lost 3, dropped 0
sink: books kept BACKGROUND on time 80985, late 473, lost 2559, dropped 11447
source: resumed the book of a dead source (resume 1): next 136541, 1269 numbers wait to be closed, issued URGENT 4873 COMMAND 159 BACKGROUND 131508
source: sent 178729 signals
source: issued URGENT 10952 COMMAND 368 BACKGROUND 167409
sink: summary URGENT received 10795, on time 10786, late 9, lost 157, dropped 0
sink: summary COMMAND received 362, on time 362, late 0, lost 6, dropped 0
sink: summary BACKGROUND received 142667, on time 142031, late 636, lost 4837, dropped 19905
sink: unaccounted 0
live-restart: URGENT issued 10952, on the sink totals 10952
live-restart: COMMAND issued 368, on the sink totals 368
live-restart: BACKGROUND issued 167409, on the sink totals 167409
live-restart: late URGENT or COMMAND inside guest stalls 9, outside 0
live-restart: ok   no restart besides the kills (4 restarts, 4 kills)
live-restart: ok   restart limit K=3 not reached (no LOCKED_SAFE)
live-restart: PASS
```

The numbers lost when relay A and relay B were killed are the ones the relays printed: relay A 2636 `lost signal` lines, relay B 2359, sink 5000. The logs of the five processes took 1.06 MB.

The panel, sampled 153 times, showed each of the four nodes with one restart, never showed a books reset, and showed both books kept. Last current sample before the stop:

```text
books kept across restarts: source 1, sink 1; 44 issued numbers wait to be closed
live-restart-check: panel URGENT issued minus accounted 0, waiting 44
live-restart-check: panel COMMAND issued minus accounted 0, waiting 44
live-restart-check: panel BACKGROUND issued minus accounted 0, waiting 44
live-restart-check: PASS
```

Guest after all runs:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

## 2026-11-24 22:31 – 2026-11-25 00:20 +06 · every late signal held against the stall probe

What changed (decisions D-064 to D-067):

- Every guest check now runs `stall-probe` beside the network and one shared script, `scripts/qnx-late-check.sh`. Each late URGENT or COMMAND gets its own line: inside a fault the check caused on purpose (SIGSTOP or a kill), inside a guest stall the probe saw (with how much stall time fell on its way), or `FAIL … NOT inside a guest stall or a fault of this check`. A late signal outside both fails the check; a check without probe output fails too. The sink prints `sink: late signal N class C at T ms, total X ns` on `CLOCK_MONOTONIC`, the clock of the probe.
- The scenario report has a `late` row per scenario. R1, R2 share, and R10 count an URGENT that was late inside a guest stall as not breaking them; the thresholds did not change (R1 share 1 and n ≥ 100, S2 median ≤ 2× S1, p99 and max ≤ 31 ms for signals outside stalls, R5 ≤ 31).
- A restarted live source takes back the dead source's mode (normal, flood, mute) from its book, or prints why it cannot.
- The counted runs (`run`, `run-flood`, `measure`) start sink, relay B, relay A, and source one after another, each after the name of the one before it exists, as the supervisor does.

One build for every result in the tables below:

```text
a8fd2ed1b58b1279b7baef8565d8e1b5d2a03fad  source
ec3e746a5f412d43cf1afd1f1e6997f3ea0f27d2  sink
0fa74595630fa136a32b9d129bbb3f41db957067  relay
997bb3c90ddeb4d0b407edec15135ca2cf77b650  supervisor
c4aca02b8f247513eea57ae44fbdcca07c7f6976  telemetry
0c1afdf45b3ed86a8dec2395e8baf225e597d435  stall-probe
```

### Five in a row

`make scenarios`, first series (23:17–23:22): exit 0, 0, 2, 0, 2.

- Run 3: S2 issued only 51 URGENT in its 5 s window (192–219 in every other run), so R2 had n < 100. None was late. The logs of that run were overwritten by the next one; the cause is not known.
- Run 5: URGENT 3948 in S2 took 31.6 ms, 0.6 ms over the deadline, and no stall over 5 ms was on its way: `S2 late-check: FAIL late URGENT 3948 NOT inside a guest stall`. 29.1 ms of it was source to relay A. Relay A spent 42.8 ms sending BACKGROUND 3741 to relay B at the same time, while the probe on the same CPU at priority 31 woke on time: the CPU was free and both relays were blocked. Six S2 runs with node logs on disk against six with logs in RAM (`/dev/shmem`) gave URGENT source-to-relay-A maxima of 7.4–14.6 ms against 6.2–9.9 ms; neither reproduced 29 ms. Not explained.

Second series (23:39–23:44), every run's logs kept: exit 0 five times, `scenarios failed 0` each time. S2 URGENT n 215, 217, 219, 217, 192; p99 7.9–9.9 ms; max 8.4–14.2 ms. In each run the only late signals were the ones the stopped relay B held in S3 (6 URGENT, 5 COMMAND), each `inside the fault stop-relay-b of this check`.

`make run-long-flood` (60 s flood): the first series passed 1–3 before the session was cut; five in a row (23:45–23:51) exit 0 each, `late URGENT or COMMAND … outside 0` each, 2358–2392 URGENT issued and the same on the sink totals. Earlier series: 5 late signals, all inside guest stalls; one of them (URGENT 1724, 29.0 ms over the deadline) had only 23.7 ms of stall on its way.

### Commands and results (one build, 23:51 – 00:13)

| Command | Exit | Result |
|---|---|---|
| `make test` | 0 | 1028 PASS, 0 FAIL |
| `make run`, `run-flood`, `run-kill`, `run-lock`, `run-source`, `run-dev`, `run-telemetry`, `panel-check` | 0 each | `unaccounted 0`; late URGENT or COMMAND 0 |
| `make run-stop` | 0 | 11 late, each inside the fault `stop-relay-b` |
| `make run-flood-hang` | 2 | `source neither exited nor gave up on the manifest within 45 s`; below |
| `make measure` (N=34) | 0 | R5 0; URGENT median calm 1014496 ns, flood 1012000 ns (p99 3374992 ns); late in 68 runs: 0 |
| `make run-long-live` (600 s) | 0 | 171286 signals, `unaccounted 0`, issued equals the sink totals; 4 late URGENT, each inside a guest stall that covers its lateness; 282 stalls over 5 ms, worst 72.8 ms |
| `make run-live-restart` (300 s, four kills) | 0 | each node restarted once, `unaccounted 0`, issued equals the sink totals (URGENT 11029, COMMAND 368, BACKGROUND 203697); `source: restored the mode of the dead source: flood`, `/dev/signal-source` `mode flood`; 2 late URGENT, inside guest stalls |

`run-flood-hang` failed once: within 45 s of relay A being stopped, source printed no `timed out` line, and its close got `Resource temporarily unavailable` instead of `Connection timed out`. Six more runs of both variants with the same build passed; source gave up 25–32 s after the stop. Likely the race of F-028: if relay A took the URGENT just before SIGSTOP, the sending thread waits for a reply and the send time limit does not free it. Not proven: no `pidin` was taken while it hung.

### URGENT under flood: B22 against B23

The B23 report gave a median of 1003992 ns against 835000 ns in B22. The B22 (`774e710`) and B23 (`4568320`) binaries were measured on the same guest in the same session, 34 flood runs per block, blocks interleaved:

| Build | URGENT median, ns | runs with relay B → sink ≈ 30–36 ms |
|---|---|---|
| B22 | 999504, 995504, 1018000, 1011992, 1004496, 990000, 995504 | 0 of 238 |
| B23 | 998000, 1050504, 1046496 | 2, 3, 4 of 34 |
| current, start order not fixed | 1068000, 1023496 | 4, 4 of 34 |

The B22 code gives the same ~1.0 ms today: the rise between the two reports was the guest, not the code. What B23 did change: in a few runs the burst reached relay B before the sink had its name, and every signal of the run waited about 30 ms there (p99 36 ms; these late URGENT were never checked by `make measure`). With the start order fixed: 0 such runs of 170 with the probe and 0 of 102 without. Over 306 URGENT each: B22 995504 ns (hops 128000 / 564000 / 283504), current without the probe 1035000 ns (132000 / 588000 / 296000), with the probe 1058000 ns. The remaining ~40 µs has no single cause found.

### F-032: the guest kernel stop

The kernel symbol file of the image (`procnto-smp-instr.sym`) places the instruction of the crash dump (`ffffff80600abad8`) in `clock_ist`, the kernel's clock interrupt thread. `nto_time@337` is the name and line of the check, not a function. The check reads `cntvct_el0`, converts it with `__cps` (62500000), and stops the kernel when the value is below the last one recorded. From the dumped registers the counter was 7488938 cycles (119.8 ms) behind on 2026-11-22 and 3624875 cycles (58.0 ms) behind on 2026-11-24. The 2026-11-22 crash has the same address and was taken before any shared memory code existed (`shm_open` first appears 2026-11-24 19:55), with `io-sock` as the active address space; on 2026-11-24 it was `devb-virtio`. The books in shared memory are therefore not needed for this crash.

10× `run-dev` and 10× `run-live-restart` (60 s, each node in turn) in a row, 23:00–23:13: no kernel stop, the serial log stayed at its 417 boot bytes, the guest answered after every run. After each clean run `/dev/shmem` held only the two relay books (98328 bytes each).

Guest after all runs:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

## 2026-11-25 01:04 – 04:00 +06 · URGENT starved by its own threads, a stuck send, and a stricter stall rule

What changed (decisions D-068 to D-071):

- **The source timer thread waits for its tick at URGENT priority.** It dropped to priority 10 before `MsgReceive`. A pulse raises only a thread already blocked in `MsgReceive`; under the S2 flood the thread stayed READY behind priority-12 work on CPU 0. `pidin -p source` during a failing S2 run showed it `10f READY` in 15 snapshots in a row while no URGENT was issued for 2.8 s.
- **Relay drop lines and drop notices go out on a separate notice thread.** The single receive thread of relay A printed one flushed line per dropped signal after each flood batch and then waited for relay B to take a drop notice; an URGENT from source waited in SEND all that time. In 17 of 18 URGENT with a source → relay A hop over 10 ms, relay A was printing drop lines until 0.3–1.3 ms before the URGENT arrived.
- **A late signal is excused by a guest stall only if the stall can cover how late it was.** The stall the probe saw is widened by one probe period (10 ms) before it, because the guest may have stopped any time after the probe's previous wake-up. Before, a stall that merely touched the signal's way excused it; 10 of 61 excused signals in all kept logs had less stall on their way than lateness, two of them in this batch's S2 runs before the relay fix.
- **source ends its run when the timer thread is stuck in a send.** `issue_live` held the issue lock through the URGENT send; when the next node took the URGENT and never answered, the flood and close threads waited on that lock and no send time limit ever fired. `make run-hold-urgent` (new, with the test program `hold-next`, which takes one URGENT and stops) reproduces it every time on the old code. The lock is now released before the send, and a watch thread ends the run when the timer thread has waited longer than the send limit.
- Every run of a check series keeps its logs: `build/scenarios/runs/<time>/`, `build/measure/runs/<time>/`, and `scripts/series.sh N TARGET` for any target.

One build for every result in the tables below:

```text
b0b9e08e363cd91e716581cbfb2a6e93419bb9e2  source
ec3e746a5f412d43cf1afd1f1e6997f3ea0f27d2  sink
4f494b628505c51cceed76bd749f3bcd6098ab99  relay
997bb3c90ddeb4d0b407edec15135ca2cf77b650  supervisor
c4aca02b8f247513eea57ae44fbdcca07c7f6976  telemetry
0c1afdf45b3ed86a8dec2395e8baf225e597d435  stall-probe
c902098679634313872a786e7a6d30d8f82d52a9  hold-next (test program)
```

### S2: URGENT issued in the 5 s flood window

`scripts/s2-series.sh` runs S2 alone and keeps each log.

| Build | S2 runs | URGENT n per run | Runs with n below 100 | Largest source → relay A hop |
|---|---|---|---|---|
| before the fixes | 15 | 57, 75, 212–220 | 2 | 6.5–15.7 ms |
| timer thread at URGENT priority | 10 | 213–220 | 0 | 7.2–9.9 ms, one run 56.2 ms |
| and relay notice thread | 10 | 213–219 | 0 | 0.5–5.4 ms |

In a failing run before the fix, no URGENT reached relay A for 3507.7 ms, until the flood was switched off. `pidin -p source` taken every ~0.35 s during another failing run:

```text
     pid tid name                         prio STATE          Blocked
445939738   1 tmp/source                    10f READY
```

(15 snapshots in a row; thread 1 is the timer thread.)

### A send that is never answered (F-033)

`make run-hold-urgent` starts `hold-next` as relay A: it takes the first URGENT and then neither replies nor receives. On the code before the fix, `pidin` 10 s after the hold:

```text
38350870   1 tmp/source                    30f REPLY          38338581
38350870   2 tmp/source                    20f RECEIVE        1
38350870   3 tmp/source                    10r MUTEX          (0x25cb914010) 38350870-01 #
38350870   4 tmp/source                    12f MUTEX          (0x25cb914010) 38350870-01 #
```

The timer thread waits for the reply; the flood and close threads wait for the lock the timer thread holds, so no send time limit fires and source never gives up (`FAIL source neither exited nor gave up on the manifest within 45 s`). After the fix:

```text
source: the next node did not take a BACKGROUND batch within 5 s; the flood thread stops
source: a signal sent 6043 ms ago has no reply: signal-relay-a took it and does not answer, and the send time limit of 5 s cannot free it; the timer thread is stuck, the run ends without it
source: sending the manifest failed: Connection timed out
hold-urgent: source gave up 26 s after the hold
hold-urgent: source exit 1 after the next node died
hold-urgent: PASS
```

The thread waiting for the reply still cannot be freed by its time limit (a `name_attach` channel only gets an unblock pulse); the process finishes exiting when the next node dies. Stopping relay A at random with SIGSTOP did not hit this window in this batch: 26 series of `run-flood-hang` and 20 runs with the earlier relay passed.

### Guest set-up: `-smp 2` against `-smp 1`

Same build, same QEMU command except `-smp`. `scripts/stall-count.sh 300` runs the stall probe on an idle guest for 300 s:

| | stalls over 5 ms | over 10 ms | over 20 ms | over 50 ms | worst | stall time |
|---|---|---|---|---|---|---|
| `-smp 2` | 1126 | 28 | 9 | 1 | 78.4 ms | 6859 ms |
| `-smp 1` | 309 | 15 | 5 | 1 | 62.3 ms | 2037 ms |

With `-smp 1` the guest stopped answering during the S2 flood and did not recover in 23 minutes; QEMU used one host core at 100 % and the serial log shows no crash. The network nodes run at SCHED_FIFO 12–30 and take the only CPU, so the scenario script, sshd, and the network stack never run again and the flood is never switched off. The guest stays on `-smp 2` (D-072). One CPU removes about 70 % of the stalls but not the long ones.

The guest kernel stopped once more on `-smp 2` in this batch, during `run-flood-hang` with relay A stopped: same instruction `ffffff80600abad8`, same check `nto_time@337` in `clock_ist`, the counter 3448164 cycles (55.2 ms) behind the time already recorded.

### Late signals and the stricter stall rule

All late URGENT that were not inside a fault of their check, over every log of this batch: 8, all from builds before the relay fix; the largest hop was relay A → relay B for 5, source → relay A for 2, relay B → sink for 1. After the fix: none in 10 S2 runs, 5 scenario series, `make measure`, `run-long-live`, and `run-long-flood`.

Under the rule of D-070, in the final runs 4 late URGENT were excused by guest stalls (1 in `run-dev`, 3 in `run-live-restart`); the stall time the probe saw covers the lateness for 3, and URGENT 113 (123.1 ms over the deadline, 119.7 ms seen) is covered only with the probe period (at most 153.7 ms).

### Commands and results (one build, guest `-smp 2`, 02:16 – 04:00)

| Command | Exit | Result |
|---|---|---|
| `make test` | 0 | 1143 PASS, 0 FAIL |
| `make scenarios` ×5 | 0 each | `scenarios failed 0` each; S2 URGENT n 224, 215, 217, 216, 214; median 1153000–1340000 ns; max 2118000–7398992 ns; late only the 6 URGENT the stopped relay B held in S3 |
| `make measure` (N=34) | 0 | R5 0; URGENT median calm 1025504 ns (p99 3350000), flood 1049496 ns (p99 3262000); late in 68 runs: 0 |
| `make run-long-live` (600 s) | 0 | 173911 signals, `unaccounted 0`, issued equals the sink totals (URGENT 22940, COMMAND 745, BACKGROUND 150226); late URGENT 0; 165 stalls over 5 ms, worst 19.2 ms |
| `make run-live-restart` (300 s, four kills) | 0 | issued URGENT 11065, COMMAND 368, BACKGROUND 203482, `unaccounted 0`; 3 late URGENT, each covered by guest stalls |
| `make run`, `run-flood`, `run-kill`, `run-lock`, `run-source`, `run-telemetry`, `panel-check` | 0 each | `unaccounted 0`; late 0 |
| `make run-stop` | 0 | 11 late, each inside the fault `stop-relay-b` |
| `make run-dev` | 0 | 1 late URGENT, covered by a guest stall |
| `make run-hold-urgent` | 0 | above |
| `make run-flood-hang` ×30 | 0 each | 30 of 30 passed; over the 60 variant runs (urgent and mute) source gave up 25 s (20) or 26 s (40) after the stop; in every one the flood thread was the one stuck |
| `make run-long-flood` ×5 | 0 each | late URGENT or COMMAND 0 in each |

Guest after all runs:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

## 2026-11-25 05:27 – 08:40 +06 · guest stalls traced to the host, URGENT no longer waits behind BACKGROUND in the relays

What changed (decisions D-073 to D-077):

- **The relay receive thread waits for the next message at URGENT priority.** It dropped to its own priority (10) after each message and then stayed READY behind the flood work at 12 on CPU 0 before it reached `MsgReceive`; an URGENT sent to it waited too. `pidin` every 0.3 s during the S2 flood (45 snapshots per relay): before, relay A's receive thread was `10f READY` in 41 snapshots and relay B's in 35; after, `30f RECEIVE` in 38 and 40, never at 10. S2 now fails when a receive thread of source or either relay rests at 10.
- **Each relay sends URGENT from its own forward thread.** With one forward thread an URGENT that arrived during a BACKGROUND send waited for that send to be answered and for the log line written after it. A second thread at URGENT priority takes only URGENT from the queue, in arrival order; the manifest waits for it.
- **source keeps one timer thread.** Measured, not assumed: the URGENT that follows a scheduled BACKGROUND send left at most 1.87 ms after its tick (119 cases).
- **Check series stop a run that hangs** (`scripts/watchdog.sh`, used by `scripts/series.sh`, every `make measure` run and every scenario) and keep the QEMU serial log when the guest does not answer.

One build for every result below (relay changed; source and sink are the B25 binaries):

```text
b0b9e08e363cd91e716581cbfb2a6e93419bb9e2  source
ec3e746a5f412d43cf1afd1f1e6997f3ea0f27d2  sink
8c2854fc44c994cb1fb4ff246bd231229607ddcb  relay
997bb3c90ddeb4d0b407edec15135ca2cf77b650  supervisor
c4aca02b8f247513eea57ae44fbdcca07c7f6976  telemetry
0c1afdf45b3ed86a8dec2395e8baf225e597d435  stall-probe
c902098679634313872a786e7a6d30d8f82d52a9  hold-next (test program)
```

### Where the guest stalls come from

`scripts/stall-host.sh` runs the stall probe on an idle guest and, at the same time, samples the Mac every 20 ms: the busy share of each core and the CPU time of each QEMU thread (`scripts/host-sample.c`, no root needed). Guest and host clocks are tied by reading the guest clock over ssh before and after; they drifted 0 ms in 300 s.

Stalls against added host load (busy `yes` processes on the Mac, 60 s each):

| Host load | stalls over 5 ms | over 10 ms | over 20 ms | worst |
|---|---|---|---|---|
| none added | 14 | 1 | 0 | 12.6 ms |
| 4 busy processes | 18 | 6 | 3 | 38.8 ms |
| 8 busy processes (all cores) | 626 | 92 | 3 | 22.0 ms |

During stalls over 20 ms in an ordinary 300 s run, the Mac cores were 77–96 % busy against 21–31 % the rest of the time, while the QEMU vCPU threads got no more CPU than usual. The guest stops when macOS does not give the QEMU vCPU thread a core.

QEMU with `-icount shift=auto` (everything else the same, same build), 300 s idle:

| | stalls over 5 ms | over 20 ms | over 50 ms | worst | stall time |
|---|---|---|---|---|---|
| without `-icount` | 148 | 12 | 1 | 62.3 ms | 1586 ms |
| with `-icount` | 2851 | 1559 | 31 | 75.5 ms | 57745 ms |

With `-icount`, `make scenarios` failed 5 of 5 (in S1 only 91 of 171 URGENT on time, median 23.3 ms) and `make measure` passed with a worse URGENT median (1588664 ns calm, 2004984 ns under flood). The guest stays without `-icount` (D-076).

### URGENT behind a send already under way (S2, 10 runs each)

| | URGENT that arrived during a COMMAND or BACKGROUND send | sent before that send ended | taken from the queue after arrival: median / max |
|---|---|---|---|
| relay A, one forward thread | 601 | 0 | 0.843 / 13.39 ms |
| relay A, URGENT thread | 593 | 593 | 0.302 / 4.10 ms |
| relay B, one forward thread | 633 | 0 | 0.262 / 8.31 ms |
| relay B, URGENT thread | 681 | 681 | 0.224 / 1.40 ms |

### URGENT gaps over 40 ms in S2

The source schedule sets the next due time as "now + period" and checks it on a 10 ms tick, so URGENT leaves every 2 or 3 ticks (873 intervals of 20 ms, 1291 of 30 ms in one series): the usual longest gap is about 31 ms. Gaps over 40 ms add a delayed issue on top. In the run traced for it, the timer thread wrote its log line for 11.7 ms and then waited 20.9 ms for relay A's receive thread, which was resting at priority 10 (fixed above). In the final S2 series, no gap over 40 ms in 10 runs.

### Commands and results (one build, guest `-smp 2` without `-icount`, 06:30 – 08:31)

| Command | Exit | Result |
|---|---|---|
| `make test` | 0 | 1170 PASS, 0 FAIL (1189 after the watchdog tests were added) |
| `make scenarios` ×5 | 0 each | `scenarios failed 0`; S2 URGENT n 251–257, median 886000–904504 ns, max 1868992–5109008 ns, R5 0; late only inside the faults of S3 |
| `scripts/s2-series.sh 10` | 0 each | n below 100 in 0 of 10 (n 249–256); median 869504–903000 ns; late 0; longest gap 31.3–32.7 ms |
| `make measure` (N=34) | 0 | R5 0; URGENT median calm 913504 ns (p99 2020000), flood 882000 ns (p99 1894000); late in 68 runs: 0 |
| `make run-long-live` (600 s) | 0 | 173480 signals, `unaccounted 0`, issued equals the sink totals (URGENT 22870, COMMAND 745, BACKGROUND 149865); late 0; 162 stalls over 5 ms, worst 18.8 ms |
| `make run-live-restart` (300 s, four kills) | 0 | each node restarted once, `unaccounted 0`, issued equals the sink totals (URGENT 11171); 2 late URGENT, inside guest stalls; the panel showed every restart |
| `make run`, `run-flood`, `run-kill`, `run-lock`, `run-stop`, `run-source`, `run-dev`, `run-telemetry`, `panel-check`, `run-hold-urgent` | 0 each | `unaccounted 0` |
| `make run-flood-hang` ×30 | 0 each | 30 of 30 passed; source gave up 25–32 s after the stop |
| `make run-long-flood` ×5 | 0 each | late URGENT or COMMAND 0 in each |

Two failures during the checks, both before the series above was completed:

- The guest kernel stopped once more, in run 06 of a first `run-flood-hang` series (mute variant, relay A stopped, flood on): `Shutdown[1] S/C/F=5/4/3`, instruction `ffffff806007fb04` (`nto_debug_fixme@193`, the same check as on 2026-11-23), active address space `bin/sh`. Runs 01–05 had passed. The guest was restarted and the 30 runs above were done again from the start.
- The first `run-live-restart` after that restart failed: `qconn` starts at every guest boot and holds TCP 8000, so `telemetry` could not listen and the panel saw no restarts. The check now kills `qconn` first, as the other panel checks already did; the run in the table is the one after the fix.

Guest after all runs:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

## 2026-11-25 15:47 – 17:09 +06 · the simulated arm stops short of the eye; URGENT every 20 ms

What changed (decisions D-079 to D-083):

- **A simulated arm in sink** (`src/common/arm_model.c`, `arm_drive.c`, no QNX headers, host tests in `tests/test_arm.c`). Planar, three joints, links 0.40 / 0.35 / 0.10 m; approach speed 250 mm/s, braking 1.25 m/s² (25.0 mm), stop margin 10 mm. The parameters were fixed in D-079 before any run and were not changed afterwards.
- **The brake engages from the sink receive thread.** On an URGENT that carries "person in", the receive thread writes the brake time (its receive time) and a flag with atomic stores before it answers. There is no lock, no print and no wait for the model. The model thread (CPU 0, priority 15, a 2 ms absolute step) computes what follows from that time. The brake also engages when the network state does not allow motion (INIT, SAFE, LOCKED_SAFE).
- **source takes `person in`, `person out`, `arm eye`, `arm home` and `arm cycle`** on `/dev/signal-source`. The event time is the moment `io_write` takes the command. A pulse at priority 30 makes the timer thread issue the URGENT at once. COMMAND carries the joint target.
- **The source schedule no longer drifts.** The next due time is the previous due time plus the period, not now plus the period (D-081).
- **telemetry gives the arm state only to a client that sends `arm`.** The plain snapshot and the panel are unchanged.

One build for every result below:

```text
4bd87c959fac9199978a3d75b373590a45513f54  source
9db863a7360b751aa853ec550b59aea6de836dff  sink
b2eec80c8f304f2c2701525040caa1884fc1408e  relay
ae44053629a93d1a7514cf5399d4d2296ea07304  supervisor
593fe77730320b72e1481406107b2ec53cba175f  telemetry
0c1afdf45b3ed86a8dec2395e8baf225e597d435  stall-probe
0caf2c57e19370757e6cd0ade9dcbfde83a9a54c  hold-next (test program)
```

relay was rebuilt only because the signal grew from 64 to 88 bytes. Its code did not change.

### The arm stop (`make run-arm-stop`, 34 calm and 34 flooded runs, watchdog 120 s on each)

In each run, the live network starts with a quiet log and the stall probe. The arm goes home, then (in the flooded runs) `flood on`, then `arm eye`. `person in` comes 200–560 ms (random) into the approach, `person out` 0.6 s later, then `stop`. Each run ends with `unaccounted 0`. Logs: `build/arm/runs/20261125-154720/`.

| | calm | flood |
|---|---|---|
| delay from `person in` to the brake: median | 1.748 ms | 0.668 ms |
| p99 = worst run (nearest rank of 34) | 3.013 ms (calm-08) | 1.421 ms (flood-21) |
| of the median: command to URGENT issue / issue to brake | 0.35 / 1.37 ms | 0.14 / 0.53 ms |
| reaction path: median / worst | 0.437 / 0.753 mm | 0.168 / 0.355 mm |
| braking path | 25.0 mm | 25.0 mm |
| stopped before the eye / in the 10 mm margin | 34 / 34 of 34 | 34 / 34 of 34 |
| closest the tool came to rest | 30.5 mm (calm-11) | 32.4 mm (flood-11) |
| model step: median / max | 35–87 µs / 14.8 ms | 16–26 µs / 10.6 ms |
| steps over 1 ms | 154 of 33281 | 37 of 42924 |
| URGENT that arrived during a model step | 18 | 23 |
| late URGENT or COMMAND outside guest stalls and faults | 0 | 0 |

The delay is shorter under flood than in calm, for the same reason as in F-031: busy vCPUs do not sleep, and waking a sleeping guest under TCG costs about a millisecond. The model steps over 1 ms match guest stalls; the brake is computed from the receive time, so a late step only catches up.

### The model does not delay the URGENT receive

The receive thread runs at 30, above the model thread at 15, on the same CPU. Of the URGENT that arrived during a model step (18 calm, 23 flood), none was late. `make measure` N=34 was run twice on the same build: with the arm, and with `SIGNAL_ARM=0` (no model thread):

| URGENT latency (median / p99 / max, µs) | with the arm | without the arm |
|---|---|---|
| calm, total | 898.0 / 1871.0 / 1894.0 | 834.5 / 1881.0 / 1970.0 |
| calm, hop 3 (relay B → sink) | 367.5 / 738.0 / 742.0 | 364.0 / 565.0 / 615.0 |
| flood, total | 827.0 / 1902.0 / 1904.0 | 838.5 / 1928.0 / 1979.0 |
| flood, hop 3 (relay B → sink) | 373.0 / 531.0 / 585.0 | 355.5 / 617.0 / 664.0 |

The hop-3 medians differ by at most 18 µs. Calm hop-3 p99 is 173 µs higher with the arm, in 102 signals; in flood it is lower with the arm. R5 violations 0 and late 0 in all 136 runs. Logs: `build/measure-b27/` (with the arm).

### URGENT every 20 ms (S2)

| | B26 | B27 |
|---|---|---|
| URGENT in the 5 s S2 window (`scripts/s2-series.sh 10`) | 249–256 | 327–328 |
| in flood | — | 251–254 |
| longest gap | 31.3–32.7 ms | 21.4–23.9 ms |
| URGENT median | 869504–903000 ns | 971008–1072000 ns |
| URGENT max | — | 2056992–7110992 ns |

The thresholds did not change. S2 still requires every URGENT on time and n ≥ 100 (n below 100 in 0 of 10), and R5 violations 0.

### Commands and results (one build, guest `-smp 2` without `-icount`, 15:47 – 17:09)

| Command | Exit | Result |
|---|---|---|
| `make test` | 0 | 1326 PASS, 0 FAIL |
| `make run-arm-stop` (34 + 34) | 0 | above |
| `make arm-telemetry-check` | 0 | the plain client got 3 snapshots with no arm line; the `arm` client got 59 of 60 snapshots with the arm, eye distance 3.3–197.1 mm |
| `make scenarios` ×5 | 0 each | `scenarios failed 0`. S2 URGENT on time 325–328 of 325–328. Median 971000–1158000 ns, max 2104000–6804000 ns, R5 0. The only late signals were held inside the faults of S3 (stopped relay B) |
| `scripts/s2-series.sh 10` | 0 each | n below 100 in 0 of 10. Late 0, gaps over 40 ms 0 |
| `make measure` (N=34) | 0 | R5 0. URGENT median calm 898000 ns (p99 1871008), flood 827000 ns (p99 1902000). Late in 68 runs: 0 |
| `make measure` (N=34, `SIGNAL_ARM=0`) | 0 | R5 0. URGENT median calm 834496 ns, flood 838496 ns. Late 0 |
| `make run-long-live` (600 s) | 0 | 178768 signals, `unaccounted 0`. Issued equals the sink totals (URGENT 29504, COMMAND 751, BACKGROUND 148513). Late 0. 1 stall over 5 ms, worst 5.1 ms |
| `make run-live-restart` (300 s, four kills) | 0 | Each node restarted once, `unaccounted 0`. Issued URGENT 14336, COMMAND 371, BACKGROUND 206874, all accounted. Late 0. The panel showed both books kept |
| `make run`, `run-flood`, `run-kill`, `run-lock`, `run-stop`, `run-source`, `run-dev`, `run-telemetry`, `panel-check`, `run-hold-urgent` | 0 each | `unaccounted 0`. Late signals only in `run-stop`, all 11 inside its fault |
| `make run-flood-hang` ×30 | 0 each | 30 of 30 passed. source gave up 25–32 s after the stop |
| `make run-long-flood` ×5 | 0 each | `failed 0 of 5` |

The Mac load average was 1.2–4.6 during the checks. No guest kernel stop.

Guest after all runs:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

## 2026-11-25 19:10 – 21:04 +06 · the same QNX with a naive network as the control; the arm stop on both

What changed (decisions D-084 to D-086):

- **A naive network built from the same code.** `make naive` builds the same `source`, `relay` and `sink` with `-DSIGNAL_NAIVE=1` into `build/naive/`. The definition was written down before any run (D-084). Exactly three things differ: (1) every node thread and every class runs at one priority, 10, with no raising (no priority per class, no boost of the forward thread, no URGENT forward thread in the relay); (2) the relay takes its queue in arrival order, any class (`signal_queue_pop_oldest`), with no second choice before the send; (3) a full queue drops the arriving signal of any class. Unchanged: queue capacity (64), CPU pinning, the flood, the arm parameters (D-079), the sink logic (pulse, SAFE, brake on the "person in" flag), the moment source issues "person in", and the accounting of every signal. The arm model thread (priority 15) is the simulated arm itself, not a network node, and is the same in both builds.
- **The stop signal is accounted.** source lists the URGENT it issued for "person in" and every later URGENT that carried the flag; a relay prints a dropped URGENT also in quiet runs. The report gives the fate of the stop signal (delivered, late, dropped at relay A or B, lost) and how many flagged URGENT were dropped or lost before the brake, on a line of its own.
- **The track of every run for the replay.** The model records the tool's distance to the eye on a 2 ms grid of model time from the start of the approach to the stop or the touch of the eye. `scripts/arm_compare.rb --replay` writes one JSON file per run: samples, event, brake (or none), touch (or none), rest, and the stop signal's class and fate (format in D-085).
- **`make run-arm-compare`** runs both networks in one guest boot, in alternating blocks of two runs (odd blocks start with ours, even blocks with the naive one), 34 calm and 34 flooded runs of each, with the D-077 watchdog on every run (D-086).

One build of each network for every arm result below:

```text
ab43c73ef07a39caa2c3381b43f02bcf4712364d  source
b9dab0f7c6c22b52c9ea33dfbdaa7382acbdd87c  sink
030df50f4f3c7c424829f6c3bb10332ecb9c8954  relay
44b998e028ea4776544b9b172c9f3ea8c8c4dc33  naive/source
1244573dc222542fb15bcd770d115916db82161e  naive/sink
edd46fec05e1d700400319d69f085272e836b350  naive/relay
```

Every log was checked to come from the build its name says (four build lines per run: source, relay A, relay B, sink).

### The arm stop on both networks (`make run-arm-compare`, 19:27 – 19:38, one guest boot)

136 runs, 0 failed. Logs: `build/arm/runs/20261125-192727/`, replay files in its `replay/`. The Mac load average was 2.4–3.7.

| | ours, calm | naive, calm | ours, flood | naive, flood |
|---|---|---|---|---|
| runs | 34 | 34 | 34 | 34 |
| delay from `person in` to the brake: median | 1.894 ms | 3.254 ms | 0.736 ms | 7.309 ms |
| p99 = worst run (nearest rank of 34) | 2.530 ms (ours-calm-31) | 5.890 ms (naive-calm-22) | 1.142 ms (ours-flood-16) | 38.700 ms (naive-flood-16) |
| reaction path: median / worst | 0.474 / 0.632 mm | 0.814 / 1.473 mm | 0.184 / 0.285 mm | 1.827 / 9.675 mm |
| braked by the person / by SAFE / no brake | 34 / 0 / 0 | 34 / 0 / 0 | 34 / 0 / 0 | 33 / 1 / 0 |
| closest the tool came to rest | 29.4 mm (ours-calm-07) | 31.9 mm (naive-calm-21) | 31.9 mm (ours-flood-14) | 28.4 mm (naive-flood-09) |
| reached the eye | 0 of 34 | 0 of 34 | 0 of 34 | 0 of 34 |
| stop signal dropped or lost | 0 | 0 | 0 | 1 (dropped at relay A, naive-flood-16) |
| stop signal late (over 31 ms) | 0 | 0 | 0 | 2 |
| flagged URGENT dropped or lost before the brake | 0 | 0 | 0 | 1 |
| late URGENT or COMMAND outside guest stalls and faults | 0 | 0 | 0 | 323 |

Delays of the 34 naive flooded runs, sorted (ms): 0.702 0.810 0.820 0.835 0.876 0.999 1.054 1.162 1.247 1.277 1.387 1.513 2.016 2.536 4.137 7.299 7.309 7.704 7.721 7.964 12.033 13.531 14.646 16.440 19.729 23.254 23.851 24.408 30.712 30.868 31.833 32.561 36.678 38.700. For naive-flood-31 the delay is the SAFE brake (7.704 ms after the event): URGENT 1667, issued before the event, arrived 39 ms after its issue, and the sink went to SAFE, as it does in both networks.

**The naive network never let the tool reach the eye.** Nothing was tuned for this. The reasons are in the setup, fixed before the runs: every URGENT carries the zone flag every 20 ms while the person is in the zone, so a dropped or late stop signal is followed by the next one (naive-flood-16: the stop signal was dropped at relay A, the next flagged URGENT braked the arm 38.7 ms after the event); the event comes 60–150 mm from the eye and the braking path is 25 mm, so even 40 ms of delay (10 mm at 250 mm/s) still stops the arm short. The naive network shows its cost as delay and lateness: under flood its median delay is ten times ours and its worst 34 times ours, and 323 URGENT and COMMAND were late outside guest stalls, against 0 for ours. In calm both networks stop the arm within a few milliseconds.

**Worst run of each network** (D-086: closest to the eye; among runs that reached it, the latest brake): ours — **ours-calm-07**, rest 29.4 mm from the eye, delay 2.235 ms, stop signal delivered. Naive — **naive-flood-09**, rest 28.4 mm, delay 14.646 ms, stop signal delivered. The run with the longest delay of the naive network is naive-flood-16 (38.7 ms, stop signal dropped at relay A, rest 89.7 mm from the eye: the event came 124 mm out).

The guest probe saw no stall behind any late signal in these runs: all 323 late signals of the naive network were outside guest stalls.

Before the series, four trial runs (one per network and mode, `build/b28/smoke/`) checked the new lines; they are not in the numbers above.

### Our network after the change (checks)

| Command | Exit | Result |
|---|---|---|
| `make test` | 0 | 1407 PASS, 0 FAIL |
| `make scenarios` ×5 | 0 each | `scenarios failed 0`. S2 URGENT on time 327–331 of 327–331. Median 987008–1004992 ns, max 2218000–3402992 ns, R5 0. Late signals only inside the faults of S3 (stopped relay B) |
| `scripts/s2-series.sh 10` | 0 each | n below 100 in 0 of 10, n 325–328, longest gap 21.4–21.8 ms, late 0 |
| `make run`, `run-flood`, `run-kill`, `run-lock`, `run-stop`, `run-source`, `run-dev`, `run-telemetry`, `panel-check`, `run-hold-urgent` | 0 each | `unaccounted 0`. Late signals only in `run-stop`, all 11 inside its fault |
| `make run-flood-hang` ×30 | 28 passed, **2 failed** | both failures are guest kernel stops (F-032) in the mute variant with relay A under `SIGSTOP`: run 04 of the first series (`nto_process@1132`), run 02 of the second (`procmgr_termer@34`). The guest was restarted twice; the 30 tries are 4 + 2 + 24. source gave up 25–32 s after the stop in the runs that passed |
| `make run-long-flood` ×5 | 0 each | `failed 0 of 5` |
| `make measure` (N=34) | 0 | R5 0. URGENT median calm 805496 ns (p99 1814000), flood 784008 ns (p99 1850992). Late in 68 runs: 0 |
| `make run-long-live` (600 s) | 0 | 179816 signals, `unaccounted 0`. Issued equals the sink totals (URGENT 29542, COMMAND 752, BACKGROUND 149522). 4 late URGENT, all inside guest stalls; 111 stalls over 5 ms, worst 49.7 ms |
| `make run-live-restart` (300 s, four kills) | 0 | Each node restarted once, `unaccounted 0`, PASS. Issued URGENT 14334, COMMAND 371, BACKGROUND 203850, every issued signal accounted or waiting to be closed. 1 late URGENT, inside a guest stall. The panel showed both books kept |

The arm series, scenarios, S2 series and single checks ran on the guest boot of 07:21; `run-flood-hang` stopped the guest kernel twice, and the later checks ran on the boot of 20:09.

Guest after all runs:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

## 2026-11-25 21:38 – 23:40 +06 · the planned stop at the eye: margin from the URGENT deadline, delay as extra path

What changed (decisions D-089 to D-091):

- **A stop margin taken from the network's promise, written down before any run.** The margin is the URGENT deadline times the approach speed: 31 ms (D-058) × 250 mm/s (D-079) = 7.75 mm. The planned stop point is 7.75 mm from the eye. "person in" is issued when the tool is 7.75 + 25.0 mm (the braking path) = 32.75 mm from the eye, so with no delay the arm rests at the planned point and every millisecond of delay is 0.25 mm past it. A chain that keeps its deadline cannot touch the eye; a chain that misses it does. Checked with the arm model on the host before the runs: the event comes 668 141 101 ns into the approach; with no delay the tool rests 7.754 mm from the eye (4 µm is the curvature of the tool path); 31 ms of delay leaves 0.004 mm, 35 ms touches. The arm parameters of D-079 are unchanged; `run-arm-stop` and `run-arm-compare` keep their random event.
- **The event at the computed instant, the same for both networks.** When the arm model starts the approach, the sink writes the start time to a small shared memory object (`/signal-sink-arm`). `echo 'person plan' > /dev/signal-source` wakes a zone sensor thread in source (URGENT priority: 30 in our build, 10 in the naive one, as every source thread there); it reads the start from shared memory, not over the network, sleeps to start + 668.141101 ms with `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` and issues "person in" with that instant as the event time. The time the thread wakes late counts in the delay to the brake. Every run checks, from the model's own track, that the tool was 32.75 ± 0.05 mm from the eye at the event; a run off by more fails and is left out of the numbers (D-091).
- **Every URGENT issued is listed after stop**, so losses in a row can be counted exactly.
- **`make run-arm-precise`**: both networks in one guest boot in alternating blocks: ours 34 calm and 34 flooded, naive 34 calm and 100 flooded (6 naive flooded runs a block, so the extra runs are spread over the series). D-077 watchdog on every run; a replay file per run in the D-085 format plus `plan_point_mm`, `event_eye_mm`, `extra_path_mm`, `past_plan_mm` and the fate of every flagged URGENT up to the brake.

One build of each network for every arm result below:

```text
395da8a708fe82c6d2deb5b55db31cbc715864fd  source
ec5cc20d70f870ec9ab44af84b8becf21487ffbc  sink
030df50f4f3c7c424829f6c3bb10332ecb9c8954  relay
0b86b41d09e096b878071d684766c91b465256cf  naive/source
443bce1cd1f8ce3d83bf69a9a099a9c1c6df2e53  naive/sink
edd46fec05e1d700400319d69f085272e836b350  naive/relay
```

### The planned stop on both networks (`make run-arm-precise`, 21:53 – 22:10, one guest boot)

202 runs, 0 failed on the guest. Logs: `build/arm/runs/20261125-215226/`, replay files in its `replay/`. The Mac load average was 2.8–3.5 before and after. Four trial runs before the series (`build/b29/smoke/`) are not in the numbers.

The tool at the event: 32.750 mm from the eye with an error of 0.000 mm in every run of ours and of the naive network at rest; at most 0.034 mm in the naive flooded runs, except three. In `naive-flood-18`, `-48` and `-89` the tool was 39.019, 35.487 and 37.622 mm away: the naive network had gone to SAFE for 21–67 ms at the start of or during the approach (a late pulse), the state brake held the arm and it fell behind the plan. These three runs fail the check and are left out of the table (D-091).

| | ours, calm | naive, calm | ours, flood | naive, flood |
|---|---|---|---|---|
| runs in the numbers | 34 | 34 | 34 | 97 (3 failed the event check) |
| delay from the event to the brake: median | 2.865 ms | 2.658 ms | 2.231 ms | 23.549 ms |
| p99 (nearest rank) / worst | 4.048 / 4.048 ms (ours-calm-20) | 4.273 / 4.273 ms (naive-calm-27) | 3.360 / 3.360 ms (ours-flood-11) | 35.194 / 35.194 ms (naive-flood-75) |
| extra path (0.25 mm per ms): median / p99 / worst | 0.72 / 1.01 / 1.01 mm | 0.66 / 1.07 / 1.07 mm | 0.56 / 0.84 / 0.84 mm | 5.89 / 8.80 / 8.80 mm |
| rest past the planned point: median / worst | 0.71 / 1.01 mm | 0.66 / 1.06 mm | 0.55 / 0.84 mm | 5.88 / 7.75 mm (at the eye) |
| closest to the eye | 6.74 mm | 6.68 mm | 6.91 mm | 0.00 mm |
| **touched the eye** | **0 of 34** | **0 of 34** | **0 of 34** | **16 of 97** |
| stop signal delivered / late / dropped | 34 / 0 / 0 | 34 / 0 / 0 | 34 / 0 / 0 | 92 / 4 / 1 (relay A) |
| zone sensor woke after the planned instant: median / worst | 1.61 / 2.95 ms | 1.43 / 3.09 ms | 1.37 / 2.64 ms | 1.70 / 3.44 ms |
| late URGENT or COMMAND outside guest stalls and faults | 0 | 0 | 0 | 381 |

**Every touch is a delay over the URGENT deadline.** The 16 touches had delays of 31.174 to 35.194 ms; every naive flooded run without a touch had at most 30.972 ms. The boundary is the deadline, as D-089 says. In 11 of the 16 touches the stop signal itself arrived on time: the deadline counts from the issue, the delay from the event, and between them are the zone sensor's wake-up (median 1.4–1.7 ms in every mode) and the issue by the timer thread. That part is the same in both networks; in ours it is more than half of the whole delay.

Delays of the 16 touches (ms): 31.174 31.224 31.276 31.288 31.387 31.792 31.835 32.089 32.261 32.441 33.264 33.416 33.610 33.732 34.882 35.194. Stop signal late in 4 of them (naive-flood-38, -50, -62, -75).

**Runs for the video** (D-086 for ours; for the naive network the touch with the latest brake): ours — **ours-flood-11**, rest 6.91 mm from the eye, 0.84 mm past the planned point, delay 3.360 ms, stop signal delivered. Naive — **naive-flood-75**, touched the eye, delay 35.194 ms (extra path 8.80 mm, more than the 7.75 mm margin), stop signal 2262 late.

**URGENT lost in a row** (naive network under flood): B28, 34 runs — 3 of 4937 URGENT dropped at relay A, one per run (the sink summary; no run had two); B29, 100 runs — 2 of 15535, by the list of every issued URGENT, both alone. Together 5 of 20472, all streaks of 1; no streak of 2 or more was seen in 20472 URGENT. A streak of n adds n URGENT periods before the next flagged URGENT: 1 → 20 ms (5 mm at 250 mm/s), 2 → 40 ms (10 mm), 3 → 60 ms (15 mm). In naive-flood-97 the stop signal 2188 was dropped at relay A; the arm was braked 3.3 ms after the event by URGENT 2115, a scheduled URGENT that already carried the zone flag.

### Our network after the change (checks)

| Command | Exit | Result |
|---|---|---|
| `make test` | 0 | 1459 PASS, 0 FAIL |
| `make scenarios` ×5 | 0 each | `scenarios failed 0`. S2 URGENT n 324–325, median 945000–1026000 ns, max 2034000–12290992 ns, R5 0 |
| `scripts/s2-series.sh 10` | 0 each | n below 100 in 0 of 10, n 322–326. Gaps over 40 ms in runs 04, 05, 07 (up to 83.4 ms); late only in run 07: 3, all inside guest stalls |
| `make run`, `run-flood`, `run-kill`, `run-lock`, `run-stop`, `run-source`, `run-dev`, `run-telemetry`, `panel-check`, `run-hold-urgent` | 0 each | `unaccounted 0` wherever it is printed. Late signals only in `run-stop`, all 11 inside its fault |
| `make run-arm-stop` | 0 | calm: delay median 2.472 ms, worst 3.903 ms, 34 of 34 short of the eye and in the 10 mm margin; flood: 0.723 / 1.048 ms, 34 of 34. Late outside stalls 0 |
| `make run-flood-hang` ×5 | 4 passed, **1 failed** | the failure is a guest kernel stop (F-032) in the mute variant with relay A under `SIGSTOP`, run 02 (`nto_process@1132`, the same place as in B28). The guest was restarted; the 5 tries are 2 + 3 |
| `make run-long-flood` ×5 | 0 each | `failed 0 of 5` |
| `make measure` (N=34) | 0 | R5 0, late 0 of 68. URGENT median calm 1699504 ns (p99 5278992), flood 1759000 ns (p99 4388000): **over the 1.19 ms flood ceiling of D-051**. See the A/B below |
| `make run-long-live` (600 s) | 0 | 184680 signals, `unaccounted 0`; issued equals the sink totals (URGENT 29442, COMMAND 750, BACKGROUND 154488). 6 late URGENT or COMMAND, all inside guest stalls; 334 stalls over 5 ms, worst 115.9 ms |
| `make run-live-restart` (300 s, four kills) | 0 | PASS; every issued URGENT, COMMAND and BACKGROUND is accounted or waits to be closed |

**`make measure` A/B on one guest boot, right after the checks (23:34–23:40):** the B28 build (`2ef8f06`, rebuilt from a worktree; SHA-1 equal to the B28 table) and this batch's build in turn, N=34 each. URGENT median calm / flood: B28 896504 / 846504 ns, B29 857496 / 849504 ns, B28 866992 / 905504 ns, B29 801496 / 788000 ns. R5 0 and late 0 in all four. Both builds are under 1.19 ms; the difference between the builds is smaller than between two runs of one build. The 1.70–1.76 ms of the measure in the chain is not this batch's code; it ran right after `run-long-flood` with the Mac at a load average of 5–7, and the cause was not measured further.

The guest kernel stopped twice during the checks: `make scenarios` run 02 at 22:18 (`nto_time@2968`, a new place, active address space `tmp/sink`) and `run-flood-hang` run 02 at 22:50 (`nto_process@1132`). After the first stop the guest booted without its disk: the QEMU descriptor `disk-qemu.vmdk` was cut at 512 bytes (F-035); it was rewritten with the person's consent and the guest booted at 22:29. The checks above all ran on the boots of 22:29 and 22:59; the first `make scenarios` series (1 of 2 passed before the stop) is not in the table. The arm series ran on the boot of 20:09. The Mac load average was 4.5–7 during the checks; D-076 asks for a free host, and the stall counts (334 in `run-long-live` against 111 in B28) show it was not.

Guest after all runs:

```text
QNX qnx-a64 8.0.0 2026/06/05-16:21:14EDT QEMU_virt aarch64le
```

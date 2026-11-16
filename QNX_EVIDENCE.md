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

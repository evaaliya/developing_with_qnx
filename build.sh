#!/bin/bash
set -euo pipefail

source /usr/local/qnx/env/bin/activate
source "$HOME/qnx800/qnxsdp-env.sh"

# Real path under the HOME mount (Docker on macOS does not follow host symlinks well).
cd "$HOME/Documents/GitHub/vsc-developing-r01-exercises-1767971935/hello"

# Match the exercise Makefile defaults (x86_64). Switch TARGET to
# -Vgcc_ntoaarch64le if you need aarch64.
qcc -Vgcc_ntox86_64 -g -Wall -o hello hello.c

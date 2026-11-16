#!/bin/bash
source /usr/local/qnx/env/bin/activate
source "$HOME/qnx800/qnxsdp-env.sh"

cd "$(dirname "$0")"

ntoaarch64-gcc -o my-project my-project.c

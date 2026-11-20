#!/bin/bash
source /usr/local/qnx/env/bin/activate
source "$HOME/qnx800/qnxsdp-env.sh"
set -eu

cd "$(dirname "$0")"
mkdir -p build

CFLAGS=(-std=c11 -Wall -Wextra -Werror -I src/common)
ntoaarch64-gcc "${CFLAGS[@]}" -o build/sink src/sink/sink.c src/common/signal_msg.c
ntoaarch64-gcc "${CFLAGS[@]}" -o build/source src/source/source.c src/common/signal_msg.c
ntoaarch64-gcc "${CFLAGS[@]}" -o build/relay \
    src/relay/relay.c src/common/signal_msg.c src/common/signal_queue.c

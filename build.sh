#!/bin/bash
source /usr/local/qnx/env/bin/activate
source "$HOME/qnx800/qnxsdp-env.sh"
set -eu

cd "$(dirname "$0")"
mkdir -p build

CFLAGS=(-std=c11 -Wall -Wextra -Werror -I src/common)
ntoaarch64-gcc "${CFLAGS[@]}" -o build/sink \
    src/sink/sink.c src/common/signal_msg.c src/common/net_state.c \
    src/common/signal_ledger.c src/common/issued_log.c
ntoaarch64-gcc "${CFLAGS[@]}" -o build/source \
    src/source/source.c src/common/signal_msg.c src/common/issue_order.c \
    src/common/issued_log.c src/common/source_ctl.c
ntoaarch64-gcc "${CFLAGS[@]}" -o build/relay \
    src/relay/relay.c src/common/signal_msg.c src/common/signal_queue.c
ntoaarch64-gcc "${CFLAGS[@]}" -o build/supervisor \
    src/supervisor/supervisor.c src/common/restart_limit.c \
    src/common/node_fault.c src/common/issued_log.c \
    src/common/signal_msg.c

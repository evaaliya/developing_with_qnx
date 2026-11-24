#!/bin/bash
source /usr/local/qnx/env/bin/activate
source "$HOME/qnx800/qnxsdp-env.sh"
set -eu

cd "$(dirname "$0")"
mkdir -p build

CFLAGS=(-std=c11 -Wall -Wextra -Werror -I src/common)
ntoaarch64-gcc "${CFLAGS[@]}" -o build/sink \
    src/sink/sink.c src/common/signal_msg.c src/common/net_state.c \
    src/common/signal_ledger.c src/common/issued_log.c \
    src/common/telemetry_snap.c src/common/telemetry_pub.c \
    src/common/latency_stats.c src/common/node_store.c -lsocket
ntoaarch64-gcc "${CFLAGS[@]}" -o build/source \
    src/source/source.c src/common/signal_msg.c src/common/issue_order.c \
    src/common/issued_log.c src/common/source_ctl.c \
    src/common/net_state.c src/common/telemetry_snap.c \
    src/common/telemetry_pub.c src/common/latency_stats.c \
    src/common/close_list.c src/common/live_book.c src/common/node_store.c \
    -lsocket
ntoaarch64-gcc "${CFLAGS[@]}" -o build/relay \
    src/relay/relay.c src/common/signal_msg.c src/common/signal_queue.c \
    src/common/close_list.c src/common/drop_book.c src/common/node_store.c \
    src/common/drop_notes.c
ntoaarch64-gcc "${CFLAGS[@]}" -o build/supervisor \
    src/supervisor/supervisor.c src/common/restart_limit.c \
    src/common/node_fault.c src/common/issued_log.c \
    src/common/signal_msg.c src/common/net_state.c \
    src/common/telemetry_snap.c src/common/telemetry_pub.c \
    src/common/latency_stats.c -lsocket
ntoaarch64-gcc "${CFLAGS[@]}" -o build/telemetry \
    src/telemetry/telemetry.c src/common/telemetry_snap.c \
    src/common/latency_stats.c src/common/net_state.c \
    src/common/signal_msg.c -lsocket
ntoaarch64-gcc "${CFLAGS[@]}" -o build/stall-probe src/stall-probe/stall_probe.c
# Test tool: a next node that takes one URGENT and never answers (F-033).
ntoaarch64-gcc "${CFLAGS[@]}" -o build/hold-next tests/qnx/hold_next.c

#!/bin/bash
source /usr/local/qnx/env/bin/activate
source "$HOME/qnx800/qnxsdp-env.sh"
set -eu

cd "$(dirname "$0")"
mkdir -p build

CFLAGS=(-std=c11 -Wall -Wextra -Werror -I src/common)

SINK_SRC=(src/sink/sink.c src/common/signal_msg.c src/common/net_state.c
    src/common/signal_ledger.c src/common/issued_log.c
    src/common/telemetry_snap.c src/common/telemetry_pub.c
    src/common/latency_stats.c src/common/node_store.c
    src/common/arm_model.c src/common/arm_drive.c -lsocket -lm)
SOURCE_SRC=(src/source/source.c src/common/signal_msg.c src/common/issue_order.c
    src/common/issued_log.c src/common/source_ctl.c
    src/common/net_state.c src/common/telemetry_snap.c
    src/common/telemetry_pub.c src/common/latency_stats.c
    src/common/close_list.c src/common/live_book.c src/common/node_store.c
    -lsocket)
RELAY_SRC=(src/relay/relay.c src/common/signal_msg.c src/common/signal_queue.c
    src/common/close_list.c src/common/drop_book.c src/common/node_store.c
    src/common/drop_notes.c)

# The naive network (D-084): the same source, relay and sink, built with
# -DSIGNAL_NAIVE=1: one priority, the relay queue in arrival order, and
# the arriving signal of any class dropped when the queue is full.
if [ "${1:-}" = naive ]; then
    mkdir -p build/naive
    NAIVE=("${CFLAGS[@]}" -DSIGNAL_NAIVE=1)
    ntoaarch64-gcc "${NAIVE[@]}" -o build/naive/sink "${SINK_SRC[@]}"
    ntoaarch64-gcc "${NAIVE[@]}" -o build/naive/source "${SOURCE_SRC[@]}"
    ntoaarch64-gcc "${NAIVE[@]}" -o build/naive/relay "${RELAY_SRC[@]}"
    exit 0
fi

ntoaarch64-gcc "${CFLAGS[@]}" -o build/sink "${SINK_SRC[@]}"
ntoaarch64-gcc "${CFLAGS[@]}" -o build/source "${SOURCE_SRC[@]}"
ntoaarch64-gcc "${CFLAGS[@]}" -o build/relay "${RELAY_SRC[@]}"
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

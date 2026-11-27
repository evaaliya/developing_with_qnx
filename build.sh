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
    src/common/arm_model.c src/common/arm_drive.c src/common/arm_plan.c
    -lsocket -lm)
SOURCE_SRC=(src/source/source.c src/common/signal_msg.c src/common/issue_order.c
    src/common/issued_log.c src/common/source_ctl.c
    src/common/net_state.c src/common/telemetry_snap.c
    src/common/telemetry_pub.c src/common/latency_stats.c
    src/common/close_list.c src/common/live_book.c src/common/node_store.c
    src/common/arm_model.c src/common/arm_plan.c -lsocket -lm)
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

# The "QNX tools" network (D-098): its own programs in src/qnx-tools/,
# sharing with Nerve only wire.h, the arm model and the event plan.
# launch links libham from the HAM package installed in the SDP (F-037).
if [ "${1:-}" = qnx-tools ]; then
    mkdir -p build/qnx-tools
    QT=("${CFLAGS[@]}" -I src/qnx-tools)
    QT_CORE=(src/qnx-tools/qt_core.c src/common/signal_msg.c)
    ntoaarch64-gcc "${QT[@]}" -o build/qnx-tools/source \
        src/qnx-tools/qt_source.c "${QT_CORE[@]}" src/common/source_ctl.c \
        src/common/arm_model.c src/common/arm_plan.c -lm
    ntoaarch64-gcc "${QT[@]}" -o build/qnx-tools/relay \
        src/qnx-tools/qt_relay.c "${QT_CORE[@]}"
    ntoaarch64-gcc "${QT[@]}" -o build/qnx-tools/sink \
        src/qnx-tools/qt_sink.c "${QT_CORE[@]}" src/common/node_store.c \
        src/common/arm_model.c src/common/arm_drive.c src/common/arm_plan.c -lm
    ntoaarch64-gcc "${QT[@]}" -I "$QNX_TARGET/usr/include" \
        -o build/qnx-tools/launch src/qnx-tools/qt_launch.c \
        -L "$QNX_TARGET/aarch64le/usr/lib" -lham
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

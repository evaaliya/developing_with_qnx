#
# Host make orchestrates the QNX tools inside the qnx-build container
# and the tests that run on the host.
#

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
# The container mounts the Mac home directory at /home/$(USER).
PROJECT_DIR := $(patsubst $(HOME)/%,/home/$(USER)/%,$(ROOT))

SSH := ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes
SCP := scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes

.PHONY: default all clean upload run test run-kill run-lock run-stop run-source run-flood run-dev run-telemetry measure stall-check

build/latency-report: src/latency-report/latency_report.c \
		src/common/latency_stats.c src/common/latency_stats.h
	mkdir -p build
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o $@ \
		src/latency-report/latency_report.c src/common/latency_stats.c

CHAIN_URGENT := 3
CHAIN_COMMAND := 3
CHAIN_BACKGROUND := 3
FLOOD_URGENT := 3
FLOOD_COMMAND := 3
FLOOD_BACKGROUND := 40
FLOOD_QUEUE := 8
# 34 calm runs and 34 flood runs: 3 URGENT each, so the 99th percentile
# has at least 100 samples (D-045). A non-zero guest status or a log
# without "sink: unaccounted 0" stops the target. That run is kept.
MEASURE_N ?= 34
# Calm runs with the sink free, then the same number pinned to CPU 0 (D-049).
STALL_K ?= 20

default:
	mkdir -p build
	docker exec qnx-build "$(PROJECT_DIR)/build.sh"

all: default

test: build/latency-report
	mkdir -p build
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-deadline \
		tests/test_deadline.c src/common/signal_msg.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-queue \
		tests/test_queue.c src/common/signal_queue.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-net-state \
		tests/test_net_state.c src/common/net_state.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-restart \
		tests/test_restart.c src/common/restart_limit.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-ledger \
		tests/test_ledger.c src/common/signal_ledger.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-latency-stats \
		tests/test_latency_stats.c src/common/latency_stats.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-issue-order \
		tests/test_issue_order.c src/common/issue_order.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-fault \
		tests/test_fault.c src/common/node_fault.c src/common/issued_log.c \
		src/common/restart_limit.c src/common/signal_msg.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-source-ctl \
		tests/test_source_ctl.c src/common/source_ctl.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-telemetry \
		tests/test_telemetry.c src/common/telemetry_snap.c \
		src/common/latency_stats.c src/common/net_state.c \
		src/common/signal_msg.c
	./build/test-deadline
	./build/test-queue
	./build/test-net-state
	./build/test-restart
	./build/test-ledger
	./build/test-latency-stats
	./build/test-issue-order
	./build/test-fault
	./build/test-source-ctl
	./build/test-telemetry
	./build/latency-report tests/fixtures/sink-latency-a.txt \
		tests/fixtures/sink-latency-b.txt > build/sink-latency.out
	diff -u tests/fixtures/sink-latency.expected build/sink-latency.out
	./build/latency-report tests/fixtures/order-stall.txt \
		tests/fixtures/order-clean.txt tests/fixtures/order-edge.txt \
		tests/fixtures/order-ahead.txt > build/order-latency.out
	diff -u tests/fixtures/order-latency.expected build/order-latency.out
	./build/latency-report --stalls tests/fixtures/order-stall.txt \
		tests/fixtures/order-clean.txt tests/fixtures/order-edge.txt \
		tests/fixtures/order-ahead.txt > build/order-stalls.out
	diff -u tests/fixtures/order-stalls.expected build/order-stalls.out

clean:
	rm -rf build
	rm -f my-project

upload: default
	$(SSH) root@localhost "slay -Q -f supervisor >/dev/null 2>&1 || true; slay -Q -f telemetry >/dev/null 2>&1 || true; slay -Q -f sink >/dev/null 2>&1 || true; slay -Q -f relay >/dev/null 2>&1 || true; slay -Q -f source >/dev/null 2>&1 || true"
	$(SCP) ./build/source ./build/sink ./build/relay ./build/supervisor ./build/telemetry root@localhost:/tmp/
	$(SCP) ./scripts/qnx-scenario.sh root@localhost:/tmp/qnx-scenario.sh

run: upload
	$(SSH) root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh calm $(CHAIN_URGENT) $(CHAIN_COMMAND) $(CHAIN_BACKGROUND)'

run-flood: upload
	$(SSH) root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh flood $(FLOOD_URGENT) $(FLOOD_COMMAND) $(FLOOD_BACKGROUND) $(FLOOD_QUEUE)'

run-kill: upload
	$(SSH) root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/supervisor /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh kill'

run-lock: upload
	$(SSH) root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/supervisor /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh lock'

run-stop: upload
	$(SSH) root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/supervisor /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh stop'

run-source: upload
	$(SSH) root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/supervisor /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh source'

run-dev: upload
	$(SCP) ./scripts/qnx-dev.sh root@localhost:/tmp/qnx-dev.sh
	$(SSH) root@localhost 'chmod +x /tmp/qnx-dev.sh /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-dev.sh'

run-telemetry: upload
	$(SCP) ./scripts/qnx-telemetry.sh root@localhost:/tmp/qnx-telemetry.sh
	./scripts/host-telemetry.py

measure: upload build/latency-report
	mkdir -p build/measure
	rm -f build/measure/calm-*.log build/measure/flood-*.log
	case '$(MEASURE_N)' in \
	  ''|*[!0-9]*|0) echo "measure: MEASURE_N must be a positive integer" >&2; exit 2 ;; \
	esac
	n='$(MEASURE_N)'; \
	i=1; \
	while [ "$$i" -le "$$n" ]; do \
	  log=$$(printf 'build/measure/calm-%02d.log' "$$i"); \
	  set +e; \
	  $(SSH) root@localhost "chmod +x /tmp/qnx-scenario.sh /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh calm $(CHAIN_URGENT) $(CHAIN_COMMAND) $(CHAIN_BACKGROUND) $$i" > "$$log"; \
	  status=$$?; \
	  set -e; \
	  if [ "$$status" -ne 0 ]; then \
	    echo "measure: calm $$i exited $$status" >&2; \
	    exit "$$status"; \
	  fi; \
	  if ! grep -q 'sink: unaccounted 0' "$$log"; then \
	    echo "measure: calm $$i has no unaccounted 0" >&2; \
	    exit 1; \
	  fi; \
	  i=$$((i + 1)); \
	done; \
	i=1; \
	while [ "$$i" -le "$$n" ]; do \
	  log=$$(printf 'build/measure/flood-%02d.log' "$$i"); \
	  set +e; \
	  $(SSH) root@localhost "chmod +x /tmp/qnx-scenario.sh /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh flood $(FLOOD_URGENT) $(FLOOD_COMMAND) $(FLOOD_BACKGROUND) $(FLOOD_QUEUE) $$i" > "$$log"; \
	  status=$$?; \
	  set -e; \
	  if [ "$$status" -ne 0 ]; then \
	    echo "measure: flood $$i exited $$status" >&2; \
	    exit "$$status"; \
	  fi; \
	  if ! grep -q 'sink: unaccounted 0' "$$log"; then \
	    echo "measure: flood $$i has no unaccounted 0" >&2; \
	    exit 1; \
	  fi; \
	  i=$$((i + 1)); \
	done; \
	set -e; \
	echo "=== calm ==="; \
	./build/latency-report build/measure/calm-*.log; \
	echo "=== flood ==="; \
	./build/latency-report build/measure/flood-*.log

stall-check: upload build/latency-report
	mkdir -p build/stall
	rm -f build/stall/free-*.log build/stall/pinned-*.log
	case '$(STALL_K)' in \
	  ''|*[!0-9]*|0) echo "stall-check: STALL_K must be a positive integer" >&2; exit 2 ;; \
	esac
	k='$(STALL_K)'; \
	i=1; \
	while [ "$$i" -le "$$k" ]; do \
	  log=$$(printf 'build/stall/free-%02d.log' "$$i"); \
	  set +e; \
	  $(SSH) root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh calm $(CHAIN_URGENT) $(CHAIN_COMMAND) $(CHAIN_BACKGROUND)' > "$$log"; \
	  status=$$?; \
	  set -e; \
	  if [ "$$status" -ne 0 ]; then \
	    echo "stall-check: free $$i exited $$status" >&2; \
	    exit "$$status"; \
	  fi; \
	  if ! grep -q 'sink: unaccounted 0' "$$log"; then \
	    echo "stall-check: free $$i has no unaccounted 0" >&2; \
	    exit 1; \
	  fi; \
	  i=$$((i + 1)); \
	done; \
	i=1; \
	while [ "$$i" -le "$$k" ]; do \
	  log=$$(printf 'build/stall/pinned-%02d.log' "$$i"); \
	  set +e; \
	  $(SSH) root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh calm $(CHAIN_URGENT) $(CHAIN_COMMAND) $(CHAIN_BACKGROUND) pin' > "$$log"; \
	  status=$$?; \
	  set -e; \
	  if [ "$$status" -ne 0 ]; then \
	    echo "stall-check: pinned $$i exited $$status" >&2; \
	    exit "$$status"; \
	  fi; \
	  if ! grep -q 'sink: unaccounted 0' "$$log"; then \
	    echo "stall-check: pinned $$i has no unaccounted 0" >&2; \
	    exit 1; \
	  fi; \
	  i=$$((i + 1)); \
	done; \
	set -e; \
	echo "=== free ==="; \
	./build/latency-report --stalls build/stall/free-*.log; \
	echo "=== pinned ==="; \
	./build/latency-report --stalls build/stall/pinned-*.log

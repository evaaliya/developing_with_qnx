#
# Host make orchestrates the QNX tools inside the qnx-build container
# and the tests that run on the host.
#

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
# The container mounts the Mac home directory at /home/$(USER).
PROJECT_DIR := $(patsubst $(HOME)/%,/home/$(USER)/%,$(ROOT))

SSH := ssh -p 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes
SCP := scp -P 2222 -i ~/.ssh/id_ed25519 -o BatchMode=yes

.PHONY: default all clean upload run test scenarios run-kill run-lock run-stop run-source run-flood run-dev run-telemetry run-long-flood run-long-live run-live-restart run-flood-hang run-hold-urgent panel-check measure stall-check run-arm-stop arm-telemetry-check naive upload-naive run-arm-compare run-arm-precise

build/latency-report: src/latency-report/latency_report.c \
		src/common/latency_stats.c src/common/latency_stats.h
	mkdir -p build
	cc -std=c11 -Wall -Wextra -Werror -DLATENCY_STATS_CAP=32768 -I src/common -o $@ \
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
# Each measure run is killed after this many seconds; a run takes ~10 s (D-077).
MEASURE_LIMIT_S ?= 180
# Shell words put before each measure run on the guest, e.g.
# MEASURE_ENV="export SIGNAL_ARM=0;" to measure without the arm model (D-080).
MEASURE_ENV ?=
# Calm runs with the sink free, then the same number pinned to CPU 0 (D-049).
STALL_K ?= 20
# Each make measure and make stall-check keeps its logs in its own
# directory; build/<check>/latest points at the newest. Nothing is erased.
RUN_STAMP := $(shell date +%Y%m%d-%H%M%S)
MEASURE_DIR := build/measure/runs/$(RUN_STAMP)
STALL_DIR := build/stall/runs/$(RUN_STAMP)
# Arm stop series (D-082): ARM_N calm runs, then ARM_N under the flood.
# One "person in" per run; each run has the D-077 time limit.
ARM_N ?= 34
ARM_LIMIT_S ?= 120
ARM_DIR := build/arm/runs/$(RUN_STAMP)

default:
	mkdir -p build
	docker exec qnx-build "$(PROJECT_DIR)/build.sh"

all: default

# The naive network (D-084): the same source, relay and sink built with
# -DSIGNAL_NAIVE=1 into build/naive/.
naive:
	mkdir -p build/naive
	docker exec qnx-build "$(PROJECT_DIR)/build.sh" naive

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
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-close-list \
		tests/test_close_list.c src/common/close_list.c
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
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-books \
		tests/test_books.c src/common/live_book.c src/common/close_list.c \
		src/common/signal_ledger.c src/common/drop_book.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-node-store \
		tests/test_node_store.c src/common/node_store.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-drop-notes \
		tests/test_drop_notes.c src/common/drop_notes.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-arm \
		tests/test_arm.c src/common/arm_model.c src/common/arm_drive.c \
		src/common/arm_plan.c -lm
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-sched-ours \
		tests/test_naive_sched.c
	cc -std=c11 -Wall -Wextra -Werror -I src/common -DSIGNAL_NAIVE=1 \
		-o build/test-sched-naive tests/test_naive_sched.c
	./build/test-deadline
	./build/test-queue
	./build/test-net-state
	./build/test-restart
	./build/test-ledger
	./build/test-close-list
	./build/test-latency-stats
	./build/test-issue-order
	./build/test-fault
	./build/test-source-ctl
	./build/test-telemetry
	./build/test-books
	./build/test-node-store
	./build/test-drop-notes
	./build/test-arm
	./build/test-sched-ours
	./build/test-sched-naive
	sh tests/test_late_check.sh
	bash tests/test_series_watchdog.sh
	bash tests/test_arm_compare.sh
	/usr/bin/ruby tests/test_panel.rb
	/usr/bin/ruby tests/test_arm_report.rb
	/usr/bin/ruby tests/test_arm_compare.rb
	/usr/bin/ruby tests/test_arm_precise.rb
	LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 /usr/bin/ruby tests/test_scenario_report.rb
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
	$(SSH) root@localhost "slay -Q -f stall-probe >/dev/null 2>&1 || true"
	$(SCP) ./build/source ./build/sink ./build/relay ./build/supervisor ./build/telemetry ./build/stall-probe root@localhost:/tmp/
	$(SCP) ./scripts/qnx-scenario.sh ./scripts/qnx-late-check.sh root@localhost:/tmp/
	$(SSH) root@localhost "chmod +x /tmp/stall-probe /tmp/qnx-late-check.sh /tmp/qnx-scenario.sh"

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

run-long-flood: upload
	$(SCP) ./scripts/qnx-long-flood.sh root@localhost:/tmp/qnx-long-flood.sh
	$(SSH) root@localhost 'chmod +x /tmp/qnx-long-flood.sh /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-long-flood.sh 60'

run-long-live: upload
	$(SCP) ./scripts/qnx-long-live.sh root@localhost:/tmp/qnx-long-live.sh
	$(SSH) root@localhost 'chmod +x /tmp/qnx-long-live.sh /tmp/stall-probe /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-long-live.sh 600'

# The live network under supervisor with node kills (D-063).
RESTART_S ?= 300
RESTART_PLAN ?= all
run-live-restart: upload
	$(SCP) ./scripts/qnx-live-restart.sh root@localhost:/tmp/qnx-live-restart.sh
	bash scripts/live-restart-check.sh $(RESTART_S) $(RESTART_PLAN)

run-flood-hang: upload
	$(SCP) ./scripts/qnx-flood-hang.sh root@localhost:/tmp/qnx-flood-hang.sh
	$(SSH) root@localhost 'chmod +x /tmp/qnx-flood-hang.sh /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-flood-hang.sh urgent && /tmp/qnx-flood-hang.sh mute'

# source must end its run when the next node takes an URGENT and never
# answers (F-033, D-071).
run-hold-urgent: upload
	$(SCP) ./build/hold-next ./scripts/qnx-hold-urgent.sh root@localhost:/tmp/
	$(SSH) root@localhost 'chmod +x /tmp/hold-next /tmp/qnx-hold-urgent.sh /tmp/source; /tmp/qnx-hold-urgent.sh'

panel-check: upload
	bash scripts/panel-check.sh

scenarios: upload build/latency-report
	bash scripts/run-scenarios.sh

run-telemetry: upload
	$(SCP) ./scripts/qnx-telemetry.sh root@localhost:/tmp/qnx-telemetry.sh
	./scripts/host-telemetry.py

measure: upload build/latency-report
	mkdir -p $(MEASURE_DIR)
	ln -sfn runs/$(RUN_STAMP) build/measure/latest
	echo "measure: logs in $(MEASURE_DIR)"
	case '$(MEASURE_N)' in \
	  ''|*[!0-9]*|0) echo "measure: MEASURE_N must be a positive integer" >&2; exit 2 ;; \
	esac
	n='$(MEASURE_N)'; \
	i=1; \
	while [ "$$i" -le "$$n" ]; do \
	  log=$$(printf '$(MEASURE_DIR)/calm-%02d.log' "$$i"); \
	  set +e; \
	  bash scripts/watchdog.sh $(MEASURE_LIMIT_S) calm-$$i $(MEASURE_DIR) $(SSH) root@localhost "$(MEASURE_ENV) chmod +x /tmp/qnx-scenario.sh /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh calm $(CHAIN_URGENT) $(CHAIN_COMMAND) $(CHAIN_BACKGROUND) $$i" > "$$log"; \
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
	  log=$$(printf '$(MEASURE_DIR)/flood-%02d.log' "$$i"); \
	  set +e; \
	  bash scripts/watchdog.sh $(MEASURE_LIMIT_S) flood-$$i $(MEASURE_DIR) $(SSH) root@localhost "$(MEASURE_ENV) chmod +x /tmp/qnx-scenario.sh /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh flood $(FLOOD_URGENT) $(FLOOD_COMMAND) $(FLOOD_BACKGROUND) $(FLOOD_QUEUE) $$i" > "$$log"; \
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
	./build/latency-report $(MEASURE_DIR)/calm-*.log; \
	echo "=== flood ==="; \
	./build/latency-report $(MEASURE_DIR)/flood-*.log; \
	echo "=== late (D-064) ==="; \
	grep -h -E '^late-check: late (URGENT|COMMAND) [0-9]+ inside' $(MEASURE_DIR)/calm-*.log $(MEASURE_DIR)/flood-*.log || true; \
	awk '/^late-check: late URGENT or COMMAND inside/ { s += $$9; f += $$12; o += $$14; n++ } END { printf "measure: late URGENT or COMMAND in %d runs: inside guest stalls %d, inside faults %d, outside %d\n", n, s, f, o }' $(MEASURE_DIR)/calm-*.log $(MEASURE_DIR)/flood-*.log

stall-check: upload build/latency-report
	mkdir -p $(STALL_DIR)
	ln -sfn runs/$(RUN_STAMP) build/stall/latest
	echo "stall-check: logs in $(STALL_DIR)"
	case '$(STALL_K)' in \
	  ''|*[!0-9]*|0) echo "stall-check: STALL_K must be a positive integer" >&2; exit 2 ;; \
	esac
	k='$(STALL_K)'; \
	i=1; \
	while [ "$$i" -le "$$k" ]; do \
	  log=$$(printf '$(STALL_DIR)/free-%02d.log' "$$i"); \
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
	  log=$$(printf '$(STALL_DIR)/pinned-%02d.log' "$$i"); \
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
	./build/latency-report --stalls $(STALL_DIR)/free-*.log; \
	echo "=== pinned ==="; \
	./build/latency-report --stalls $(STALL_DIR)/pinned-*.log

# The arm stops for "person in" (D-080, D-082): delay from the event to
# the brake, reaction path, braking path, stopped before the eye. Logs
# of every run in build/arm/runs/<time>/; build/arm/latest points there.
run-arm-stop: upload
	mkdir -p $(ARM_DIR)
	ln -sfn runs/$(RUN_STAMP) build/arm/latest
	echo "run-arm-stop: logs in $(ARM_DIR)"
	case '$(ARM_N)' in \
	  ''|*[!0-9]*|0) echo "run-arm-stop: ARM_N must be a positive integer" >&2; exit 2 ;; \
	esac
	$(SCP) ./scripts/qnx-arm-stop.sh root@localhost:/tmp/qnx-arm-stop.sh
	$(SSH) root@localhost 'chmod +x /tmp/qnx-arm-stop.sh'
	n='$(ARM_N)'; \
	for mode in calm flood; do \
	  i=1; \
	  while [ "$$i" -le "$$n" ]; do \
	    log=$$(printf '$(ARM_DIR)/%s-%02d.log' "$$mode" "$$i"); \
	    set +e; \
	    bash scripts/watchdog.sh $(ARM_LIMIT_S) $$mode-$$i $(ARM_DIR) $(SSH) root@localhost "/tmp/qnx-arm-stop.sh $$mode $$i" > "$$log" 2>&1; \
	    status=$$?; \
	    set -e; \
	    if [ "$$status" -ne 0 ]; then \
	      echo "run-arm-stop: $$mode $$i exited $$status" >&2; \
	      exit "$$status"; \
	    fi; \
	    i=$$((i + 1)); \
	  done; \
	done
	/usr/bin/ruby scripts/arm_report.rb $(ARM_DIR)/calm-*.log $(ARM_DIR)/flood-*.log | tee $(ARM_DIR)/report.txt

upload-naive: naive
	$(SSH) root@localhost "slay -Q -f source >/dev/null 2>&1 || true; slay -Q -f relay >/dev/null 2>&1 || true; slay -Q -f sink >/dev/null 2>&1 || true; mkdir -p /tmp/naive"
	$(SCP) ./build/naive/source ./build/naive/sink ./build/naive/relay root@localhost:/tmp/naive/
	$(SSH) root@localhost "chmod +x /tmp/naive/source /tmp/naive/sink /tmp/naive/relay"

# The arm stop on our network and on the naive one in one guest boot, in
# alternating blocks of ARM_BLOCK runs (D-086): ARM_N calm and ARM_N
# flooded runs of each. The report names the worst run of each network;
# ARM_DIR/replay/ holds the track of every run for the replay page (D-085).
ARM_BLOCK ?= 2
run-arm-compare: upload upload-naive
	mkdir -p $(ARM_DIR)
	ln -sfn runs/$(RUN_STAMP) build/arm/latest
	echo "run-arm-compare: logs in $(ARM_DIR)"
	$(SCP) ./scripts/qnx-arm-stop.sh root@localhost:/tmp/qnx-arm-stop.sh
	$(SSH) root@localhost 'chmod +x /tmp/qnx-arm-stop.sh; uname -a' | tee $(ARM_DIR)/guest.txt
	{ bash scripts/arm-compare.sh $(ARM_N) $(ARM_BLOCK) $(ARM_LIMIT_S) $(ARM_DIR); \
	  echo $$? > $(ARM_DIR)/series.status; } | tee $(ARM_DIR)/series.txt
	{ /usr/bin/ruby scripts/arm_compare.rb --replay $(ARM_DIR)/replay \
	    $(ARM_DIR)/ours-*.log $(ARM_DIR)/naive-*.log; \
	  echo $$? > $(ARM_DIR)/report.status; } | tee $(ARM_DIR)/report.txt
	echo "run-arm-compare: series exit $$(cat $(ARM_DIR)/series.status), report exit $$(cat $(ARM_DIR)/report.status)"
	[ "$$(cat $(ARM_DIR)/series.status)" = 0 ] && [ "$$(cat $(ARM_DIR)/report.status)" = 0 ]

# The planned stop at the eye on both networks (D-089, D-090): the zone
# sensor in source issues "person in" when the tool is 7.75 + 25.0 mm from
# the eye. Alternating blocks as run-arm-compare; ARM_N calm and flooded
# runs of ours, ARM_N calm and ARM_NAIVE_FLOOD_N flooded runs of the naive
# network, spread over the blocks. The report checks the tool at the
# event, names the runs for the video, and counts URGENT lost in a row.
ARM_NAIVE_FLOOD_N ?= 100
run-arm-precise: upload upload-naive
	mkdir -p $(ARM_DIR)
	ln -sfn runs/$(RUN_STAMP) build/arm/latest
	echo "run-arm-precise: logs in $(ARM_DIR)"
	$(SCP) ./scripts/qnx-arm-stop.sh root@localhost:/tmp/qnx-arm-stop.sh
	$(SSH) root@localhost 'chmod +x /tmp/qnx-arm-stop.sh; uname -a' | tee $(ARM_DIR)/guest.txt
	{ bash scripts/arm-compare.sh $(ARM_N) $(ARM_BLOCK) $(ARM_LIMIT_S) $(ARM_DIR) $(ARM_NAIVE_FLOOD_N) plan; \
	  echo $$? > $(ARM_DIR)/series.status; } | tee $(ARM_DIR)/series.txt
	{ /usr/bin/ruby scripts/arm_precise.rb --replay $(ARM_DIR)/replay \
	    $(ARM_DIR)/ours-*.log $(ARM_DIR)/naive-*.log; \
	  echo $$? > $(ARM_DIR)/report.status; } | tee $(ARM_DIR)/report.txt
	/usr/bin/ruby scripts/arm_precise.rb --streaks $(ARM_DIR)/naive-flood-*.log | tee $(ARM_DIR)/streaks.txt
	echo "run-arm-precise: series exit $$(cat $(ARM_DIR)/series.status), report exit $$(cat $(ARM_DIR)/report.status)"
	[ "$$(cat $(ARM_DIR)/series.status)" = 0 ] && [ "$$(cat $(ARM_DIR)/report.status)" = 0 ]

# Telemetry gives the arm to a client that sends "arm" (D-080).
arm-telemetry-check: upload
	python3 scripts/arm-telemetry-check.py

#
# Host make orchestrates the QNX tools inside the qnx-build container
# and the tests that run on the host.
#

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
# The container mounts the Mac home directory at /home/$(USER).
PROJECT_DIR := $(patsubst $(HOME)/%,/home/$(USER)/%,$(ROOT))

SSH := ssh -p 2222 -i $(HOME)/.ssh/id_ed25519 -o BatchMode=yes
SCP := scp -P 2222 -i $(HOME)/.ssh/id_ed25519 -o BatchMode=yes

.PHONY: default all clean upload run test run-kill run-lock

CHAIN_URGENT := 3
CHAIN_COMMAND := 3
CHAIN_BACKGROUND := 3

default:
	mkdir -p build
	docker exec qnx-build "$(PROJECT_DIR)/build.sh"

all: default

test:
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
	./build/test-deadline
	./build/test-queue
	./build/test-net-state
	./build/test-restart
	./build/test-ledger

clean:
	rm -rf build
	rm -f my-project

upload: default
	$(SSH) root@localhost "slay -Q -f supervisor >/dev/null 2>&1 || true; slay -Q -f sink >/dev/null 2>&1 || true; slay -Q -f relay >/dev/null 2>&1 || true; slay -Q -f source >/dev/null 2>&1 || true"
	$(SCP) ./build/source ./build/sink ./build/relay ./build/supervisor root@localhost:/tmp/
	$(SCP) ./scripts/qnx-scenario.sh root@localhost:/tmp/qnx-scenario.sh

run: upload
	$(SSH) root@localhost 'chmod +x /tmp/source /tmp/sink /tmp/relay; \
	  total=$$(($(CHAIN_URGENT) + $(CHAIN_COMMAND) + $(CHAIN_BACKGROUND))); \
	  /tmp/sink $$total > /tmp/sink.out 2>&1 & \
	  sink_pid=$$!; \
	  /tmp/relay signal-relay-b signal-sink > /tmp/relay-b.out 2>&1 & \
	  relay_b=$$!; \
	  /tmp/relay signal-relay-a signal-relay-b > /tmp/relay-a.out 2>&1 & \
	  relay_a=$$!; \
	  /tmp/source $(CHAIN_URGENT) $(CHAIN_COMMAND) $(CHAIN_BACKGROUND) signal-relay-a > /tmp/source.out 2>&1; \
	  source_status=$$?; \
	  sink_status=0; \
	  wait $$sink_pid || sink_status=$$?; \
	  slay -Q -f relay >/dev/null 2>&1 || true; \
	  slay -Q -f source >/dev/null 2>&1 || true; \
	  slay -Q -f sink >/dev/null 2>&1 || true; \
	  wait $$relay_a >/dev/null 2>&1 || true; \
	  wait $$relay_b >/dev/null 2>&1 || true; \
	  echo "--- source (exit $$source_status) ---"; \
	  cat /tmp/source.out; \
	  echo "--- relay A ---"; \
	  cat /tmp/relay-a.out; \
	  echo "--- relay B ---"; \
	  cat /tmp/relay-b.out; \
	  echo "--- sink (exit $$sink_status) ---"; \
	  cat /tmp/sink.out; \
	  if [ "$$source_status" -ne 0 ]; then exit "$$source_status"; fi; \
	  exit "$$sink_status"'

run-kill: upload
	$(SSH) root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/supervisor /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh kill'

run-lock: upload
	$(SSH) root@localhost 'chmod +x /tmp/qnx-scenario.sh /tmp/supervisor /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-scenario.sh lock'

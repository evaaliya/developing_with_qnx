#
# Host make orchestrates the QNX tools inside the qnx-build container
# and the tests that run on the host.
#

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
# The container mounts the Mac home directory at /home/$(USER).
PROJECT_DIR := $(patsubst $(HOME)/%,/home/$(USER)/%,$(ROOT))

SSH := ssh -p 2222 -i $(HOME)/.ssh/id_ed25519 -o BatchMode=yes
SCP := scp -P 2222 -i $(HOME)/.ssh/id_ed25519 -o BatchMode=yes

.PHONY: default all clean upload run test

default:
	mkdir -p build
	docker exec qnx-build "$(PROJECT_DIR)/build.sh"

all: default

test:
	mkdir -p build
	cc -std=c11 -Wall -Wextra -Werror -I src/common -o build/test-deadline \
		tests/test_deadline.c src/common/signal_msg.c
	./build/test-deadline

clean:
	rm -rf build
	rm -f my-project

upload: default
	$(SSH) root@localhost "slay -Q -f sink >/dev/null 2>&1 || true; slay -Q -f source >/dev/null 2>&1 || true"
	$(SCP) ./build/source ./build/sink root@localhost:/tmp/

run: upload
	$(SSH) root@localhost 'chmod +x /tmp/source /tmp/sink; \
	  /tmp/sink > /tmp/sink.out 2>&1 & \
	  sink_pid=$$!; \
	  /tmp/source > /tmp/source.out 2>&1; \
	  source_status=$$?; \
	  if [ "$$source_status" -ne 0 ]; then slay -Q -f sink >/dev/null 2>&1 || true; fi; \
	  sink_status=0; \
	  wait $$sink_pid || sink_status=$$?; \
	  echo "--- source (exit $$source_status) ---"; \
	  cat /tmp/source.out; \
	  echo "--- sink (exit $$sink_status) ---"; \
	  cat /tmp/sink.out; \
	  if [ "$$source_status" -ne 0 ]; then exit "$$source_status"; fi; \
	  exit "$$sink_status"'

#
# Makefile for my-project (macOS + Docker QNX build)
# Host make only orchestrates the Linux QNX tools inside the qnx-build container.
#

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
# The container mounts the Mac home directory at /home/$(USER).
PROJECT_DIR := $(patsubst $(HOME)/%,/home/$(USER)/%,$(ROOT))

default:
	docker exec -it qnx-build "$(PROJECT_DIR)/build.sh"

all: default

clean:
	rm -f my-project

upload:
	scp -P 2222 -i $(HOME)/.ssh/id_ed25519 ./my-project root@localhost:/tmp/my-project

run:
	ssh -p 2222 -i $(HOME)/.ssh/id_ed25519 root@localhost "chmod +x /tmp/my-project && /tmp/my-project"

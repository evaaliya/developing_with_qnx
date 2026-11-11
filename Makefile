#
# Makefile for hello project (macOS + Docker QNX build)
# Host make only orchestrates the Linux QNX tools inside the qnx-build container.
#

PROJECT_DIR = /home/$(USER)/Documents/GitHub/vsc-developing-r01-exercises-1767971935/hello

default:
	docker exec -it qnx-build $(PROJECT_DIR)/build.sh

all: default

clean:
	rm -f *.o hello
	cd solutions; $(MAKE) clean_soln

upload:
	sshpass -p "qnxuser" scp ./hello qnxuser@192.168.1.100:~

run:
	sshpass -p "qnxuser" ssh qnxuser@192.168.1.100 "/data/home/qnxuser/hello"

CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=

INSTALL_PATH ?= /usr/local/bin
SYSTEMD_UNIT_DIR ?= /etc/systemd/system
CONFIG_PATH ?= /etc/memf.conf
BINARY := memf
SRC := src/memf.c

.PHONY: all build run install install-service uninstall-service clean

all: build

build: $(BINARY)

$(BINARY): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic -D_DEFAULT_SOURCE -o $@ $< $(LDFLAGS)

run: build
	sudo ./$(BINARY)

install: build
	install -d "$(INSTALL_PATH)"
	install -m 0755 ./$(BINARY) "$(INSTALL_PATH)/$(BINARY)"

install-service: install
	install -d "$(SYSTEMD_UNIT_DIR)"
	install -m 0644 systemd/memf.service "$(SYSTEMD_UNIT_DIR)/memf.service"
	@if [ ! -f "$(CONFIG_PATH)" ]; then install -m 0644 memf.conf.example "$(CONFIG_PATH)"; fi
	systemctl daemon-reload

uninstall-service:
	-systemctl disable --now memf.service
	rm -f "$(SYSTEMD_UNIT_DIR)/memf.service"
	systemctl daemon-reload

clean:
	rm -f ./$(BINARY)

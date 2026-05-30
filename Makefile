CC ?= cc
CFLAGS ?= -Wall -Wextra -std=c11
LDFLAGS ?=
LDLIBS ?= -lX11 -lXrandr -lm

APP := flashguard
SRC := flashguard.c
BIN := $(APP)

PREFIX ?= $(HOME)/.local
DESTDIR ?=
SYSTEMD_USER_DIR ?= $(HOME)/.config/systemd/user
XDG_CONFIG_HOME ?= $(HOME)/.config
CONFIG_DIR := $(XDG_CONFIG_HOME)/flashguard
CONFIG_FILE := $(CONFIG_DIR)/flashguard.config

.PHONY: all build clean run install install-bin install-config install-service uninstall-service enable-service disable-service restart-service logs status

all: build

build: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

run: build
	./$(BIN)

install: install-bin install-config install-service

install-bin: build
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(APP)

install-config:
	install -d $(CONFIG_DIR)
	@if [ ! -f "$(CONFIG_FILE)" ]; then \
		install -m 0644 flashguard.config "$(CONFIG_FILE)"; \
		echo "Created $(CONFIG_FILE)"; \
	else \
		echo "Keeping existing $(CONFIG_FILE)"; \
	fi

install-service:
	install -d $(SYSTEMD_USER_DIR)
	install -m 0644 flashguard.service $(SYSTEMD_USER_DIR)/flashguard.service
	systemctl --user daemon-reload

uninstall-service:
	-systemctl --user disable --now flashguard.service
	-rm -f $(SYSTEMD_USER_DIR)/flashguard.service
	-systemctl --user daemon-reload

enable-service:
	systemctl --user enable --now flashguard.service

disable-service:
	systemctl --user disable --now flashguard.service

restart-service:
	systemctl --user restart flashguard.service

status:
	systemctl --user status flashguard.service --no-pager

logs:
	journalctl --user -u flashguard.service -f

clean:
	rm -f $(BIN)
